/**
 * \file errors.c
 * Mesa debugging and error handling functions.
 */

/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 1999-2007  Brian Paul   All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */


#include "errors.h"
#include "enums.h"
#include "imports.h"
#include "context.h"
#include "dispatch.h"
#include "hash.h"
#include "mtypes.h"
#include "version.h"
#include "hash_table.h"

static mtx_t DynamicIDMutex = _MTX_INITIALIZER_NP;
static GLuint NextDynamicID = 1;

struct gl_debug_severity
{
   struct simple_node link;
   GLuint ID;
};

static char out_of_memory[] = "Debugging error: out of memory";

static const GLenum debug_source_enums[] = {
   GL_DEBUG_SOURCE_API,
   GL_DEBUG_SOURCE_WINDOW_SYSTEM,
   GL_DEBUG_SOURCE_SHADER_COMPILER,
   GL_DEBUG_SOURCE_THIRD_PARTY,
   GL_DEBUG_SOURCE_APPLICATION,
   GL_DEBUG_SOURCE_OTHER,
};

static const GLenum debug_type_enums[] = {
   GL_DEBUG_TYPE_ERROR,
   GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR,
   GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR,
   GL_DEBUG_TYPE_PORTABILITY,
   GL_DEBUG_TYPE_PERFORMANCE,
   GL_DEBUG_TYPE_OTHER,
   GL_DEBUG_TYPE_MARKER,
   GL_DEBUG_TYPE_PUSH_GROUP,
   GL_DEBUG_TYPE_POP_GROUP,
};

static const GLenum debug_severity_enums[] = {
   GL_DEBUG_SEVERITY_LOW,
   GL_DEBUG_SEVERITY_MEDIUM,
   GL_DEBUG_SEVERITY_HIGH,
   GL_DEBUG_SEVERITY_NOTIFICATION,
};


static enum mesa_debug_source
gl_enum_to_debug_source(GLenum e)
{
   int i;

   for (i = 0; i < Elements(debug_source_enums); i++) {
      if (debug_source_enums[i] == e)
         break;
   }
   return i;
}

static enum mesa_debug_type
gl_enum_to_debug_type(GLenum e)
{
   int i;

   for (i = 0; i < Elements(debug_type_enums); i++) {
      if (debug_type_enums[i] == e)
         break;
   }
   return i;
}

static enum mesa_debug_severity
gl_enum_to_debug_severity(GLenum e)
{
   int i;

   for (i = 0; i < Elements(debug_severity_enums); i++) {
      if (debug_severity_enums[i] == e)
         break;
   }
   return i;
}


/**
 * Handles generating a GL_ARB_debug_output message ID generated by the GL or
 * GLSL compiler.
 *
 * The GL API has this "ID" mechanism, where the intention is to allow a
 * client to filter in/out messages based on source, type, and ID.  Of course,
 * building a giant enum list of all debug output messages that Mesa might
 * generate is ridiculous, so instead we have our caller pass us a pointer to
 * static storage where the ID should get stored.  This ID will be shared
 * across all contexts for that message (which seems like a desirable
 * property, even if it's not expected by the spec), but note that it won't be
 * the same between executions if messages aren't generated in the same order.
 */
static void
debug_get_id(GLuint *id)
{
   if (!(*id)) {
      mtx_lock(&DynamicIDMutex);
      if (!(*id))
         *id = NextDynamicID++;
      mtx_unlock(&DynamicIDMutex);
   }
}


/*
 * We store a bitfield in the hash table, with five possible values total.
 *
 * The ENABLED_BIT's purpose is self-explanatory.
 *
 * The FOUND_BIT is needed to differentiate the value of DISABLED from
 * the value returned by HashTableLookup() when it can't find the given key.
 *
 * The KNOWN_SEVERITY bit is a bit complicated:
 *
 * A client may call Control() with an array of IDs, then call Control()
 * on all message IDs of a certain severity, then Insert() one of the
 * previously specified IDs, giving us a known severity level, then call
 * Control() on all message IDs of a certain severity level again.
 *
 * After the first call, those IDs will have a FOUND_BIT, but will not
 * exist in any severity-specific list, so the second call will not
 * impact them. This is undesirable but unavoidable given the API:
 * The only entrypoint that gives a severity for a client-defined ID
 * is the Insert() call.
 *
 * For the sake of Control(), we want to maintain the invariant
 * that an ID will either appear in none of the three severity lists,
 * or appear once, to minimize pointless duplication and potential surprises.
 *
 * Because Insert() is the only place that will learn an ID's severity,
 * it should insert an ID into the appropriate list, but only if the ID
 * doesn't exist in it or any other list yet. Because searching all three
 * lists at O(n) is needlessly expensive, we store KNOWN_SEVERITY.
 */
enum {
   FOUND_BIT = 1 << 0,
   ENABLED_BIT = 1 << 1,
   KNOWN_SEVERITY = 1 << 2,

   /* HashTable reserves zero as a return value meaning 'not found' */
   NOT_FOUND = 0,
   DISABLED = FOUND_BIT,
   ENABLED = ENABLED_BIT | FOUND_BIT
};


/**
 * Return debug state for the context.  The debug state will be allocated
 * and initialized upon the first call.
 */
struct gl_debug_state *
_mesa_get_debug_state(struct gl_context *ctx)
{
   if (!ctx->Debug) {
      ctx->Debug = CALLOC_STRUCT(gl_debug_state);
      if (!ctx->Debug) {
         _mesa_error(ctx, GL_OUT_OF_MEMORY, "allocating debug state");
      }
      else {
         struct gl_debug_state *debug = ctx->Debug;
         int s, t, sev;

         /* Enable all the messages with severity HIGH or MEDIUM by default. */
         memset(debug->Defaults[0][MESA_DEBUG_SEVERITY_HIGH], GL_TRUE,
                sizeof debug->Defaults[0][MESA_DEBUG_SEVERITY_HIGH]);
         memset(debug->Defaults[0][MESA_DEBUG_SEVERITY_MEDIUM], GL_TRUE,
                sizeof debug->Defaults[0][MESA_DEBUG_SEVERITY_MEDIUM]);
         memset(debug->Defaults[0][MESA_DEBUG_SEVERITY_LOW], GL_FALSE,
                sizeof debug->Defaults[0][MESA_DEBUG_SEVERITY_LOW]);

         /* Initialize state for filtering known debug messages. */
         for (s = 0; s < MESA_DEBUG_SOURCE_COUNT; s++) {
            for (t = 0; t < MESA_DEBUG_TYPE_COUNT; t++) {
               debug->Namespaces[0][s][t].IDs = _mesa_NewHashTable();
               assert(debug->Namespaces[0][s][t].IDs);

               for (sev = 0; sev < MESA_DEBUG_SEVERITY_COUNT; sev++) {
                  make_empty_list(&debug->Namespaces[0][s][t].Severity[sev]);
               }
            }
         }
      }
   }

   return ctx->Debug;
}



/**
 * Returns the state of the given message source/type/ID tuple.
 */
static GLboolean
should_log(struct gl_context *ctx,
           enum mesa_debug_source source,
           enum mesa_debug_type type,
           GLuint id,
           enum mesa_debug_severity severity)
{
   struct gl_debug_state *debug;
   uintptr_t state = 0;

   if (!ctx->Debug) {
      /* no debug state set so far */
      return GL_FALSE;
   }

   debug = _mesa_get_debug_state(ctx);
   if (debug) {
      const GLint gstack = debug->GroupStackDepth;
      struct gl_debug_namespace *nspace =
         &debug->Namespaces[gstack][source][type];

      if (!debug->DebugOutput)
         return GL_FALSE;

      /* In addition to not being able to store zero as a value, HashTable also
       * can't use zero as a key.
       */
      if (id)
         state = (uintptr_t)_mesa_HashLookup(nspace->IDs, id);
      else
         state = nspace->ZeroID;

      /* Only do this once for each ID. This makes sure the ID exists in,
       * at most, one list, and does not pointlessly appear multiple times.
       */
      if (!(state & KNOWN_SEVERITY)) {
         struct gl_debug_severity *entry;

         if (state == NOT_FOUND) {
            if (debug->Defaults[gstack][severity][source][type])
               state = ENABLED;
            else
               state = DISABLED;
         }

         entry = malloc(sizeof *entry);
         if (!entry)
            goto out;

         state |= KNOWN_SEVERITY;

         if (id)
            _mesa_HashInsert(nspace->IDs, id, (void*)state);
         else
            nspace->ZeroID = state;

         entry->ID = id;
         insert_at_tail(&nspace->Severity[severity], &entry->link);
      }
   }
out:
   return !!(state & ENABLED_BIT);
}


/**
 * Sets the state of the given message source/type/ID tuple.
 */
static void
set_message_state(struct gl_context *ctx,
                  enum mesa_debug_source source,
                  enum mesa_debug_type type,
                  GLuint id, GLboolean enabled)
{
   struct gl_debug_state *debug = _mesa_get_debug_state(ctx);

   if (debug) {
      GLint gstack = debug->GroupStackDepth;
      struct gl_debug_namespace *nspace =
         &debug->Namespaces[gstack][source][type];
      uintptr_t state;

      /* In addition to not being able to store zero as a value, HashTable also
       * can't use zero as a key.
       */
      if (id)
         state = (uintptr_t)_mesa_HashLookup(nspace->IDs, id);
      else
         state = nspace->ZeroID;

      if (state == NOT_FOUND)
         state = enabled ? ENABLED : DISABLED;
      else {
         if (enabled)
            state |= ENABLED_BIT;
         else
            state &= ~ENABLED_BIT;
      }

      if (id)
         _mesa_HashInsert(nspace->IDs, id, (void*)state);
      else
         nspace->ZeroID = state;
   }
}


static void
store_message_details(struct gl_debug_msg *emptySlot,
                      enum mesa_debug_source source,
                      enum mesa_debug_type type, GLuint id,
                      enum mesa_debug_severity severity, GLint len,
                      const char *buf)
{
   assert(!emptySlot->message && !emptySlot->length);

   emptySlot->message = malloc(len+1);
   if (emptySlot->message) {
      (void) strncpy(emptySlot->message, buf, (size_t)len);
      emptySlot->message[len] = '\0';

      emptySlot->length = len+1;
      emptySlot->source = source;
      emptySlot->type = type;
      emptySlot->id = id;
      emptySlot->severity = severity;
   } else {
      static GLuint oom_msg_id = 0;
      debug_get_id(&oom_msg_id);

      /* malloc failed! */
      emptySlot->message = out_of_memory;
      emptySlot->length = strlen(out_of_memory)+1;
      emptySlot->source = MESA_DEBUG_SOURCE_OTHER;
      emptySlot->type = MESA_DEBUG_TYPE_ERROR;
      emptySlot->id = oom_msg_id;
      emptySlot->severity = MESA_DEBUG_SEVERITY_HIGH;
   }
}


/**
 * 'buf' is not necessarily a null-terminated string. When logging, copy
 * 'len' characters from it, store them in a new, null-terminated string,
 * and remember the number of bytes used by that string, *including*
 * the null terminator this time.
 */
static void
log_msg(struct gl_context *ctx, enum mesa_debug_source source,
        enum mesa_debug_type type, GLuint id,
        enum mesa_debug_severity severity, GLint len, const char *buf)
{
   struct gl_debug_state *debug = _mesa_get_debug_state(ctx);
   GLint nextEmpty;
   struct gl_debug_msg *emptySlot;

   if (!debug)
      return;

   assert(len >= 0 && len < MAX_DEBUG_MESSAGE_LENGTH);

   if (!should_log(ctx, source, type, id, severity))
      return;

   if (debug->Callback) {
       GLenum gl_type = debug_type_enums[type];
       GLenum gl_severity = debug_severity_enums[severity];

      debug->Callback(debug_source_enums[source], gl_type, id, gl_severity,
                      len, buf, debug->CallbackData);
      return;
   }

   if (debug->NumMessages == MAX_DEBUG_LOGGED_MESSAGES)
      return;

   nextEmpty = (debug->NextMsg + debug->NumMessages)
                          % MAX_DEBUG_LOGGED_MESSAGES;
   emptySlot = &debug->Log[nextEmpty];

   store_message_details(emptySlot, source, type, id, severity, len, buf);

   if (debug->NumMessages == 0)
      debug->NextMsgLength = debug->Log[debug->NextMsg].length;

   debug->NumMessages++;
}


/**
 * Pop the oldest debug message out of the log.
 * Writes the message string, including the null terminator, into 'buf',
 * using up to 'bufSize' bytes. If 'bufSize' is too small, or
 * if 'buf' is NULL, nothing is written.
 *
 * Returns the number of bytes written on success, or when 'buf' is NULL,
 * the number that would have been written. A return value of 0
 * indicates failure.
 */
static GLsizei
get_msg(struct gl_context *ctx, GLenum *source, GLenum *type,
        GLuint *id, GLenum *severity, GLsizei bufSize, char *buf)
{
   struct gl_debug_state *debug = _mesa_get_debug_state(ctx);
   struct gl_debug_msg *msg;
   GLsizei length;

   if (!debug || debug->NumMessages == 0)
      return 0;

   msg = &debug->Log[debug->NextMsg];
   length = msg->length;

   assert(length > 0 && length == debug->NextMsgLength);

   if (bufSize < length && buf != NULL)
      return 0;

   if (severity) {
      *severity = debug_severity_enums[msg->severity];
   }

   if (source) {
      *source = debug_source_enums[msg->source];
   }

   if (type) {
      *type = debug_type_enums[msg->type];
   }

   if (id) {
      *id = msg->id;
   }

   if (buf) {
      assert(msg->message[length-1] == '\0');
      (void) strncpy(buf, msg->message, (size_t)length);
   }

   if (msg->message != (char*)out_of_memory)
      free(msg->message);
   msg->message = NULL;
   msg->length = 0;

   debug->NumMessages--;
   debug->NextMsg++;
   debug->NextMsg %= MAX_DEBUG_LOGGED_MESSAGES;
   debug->NextMsgLength = debug->Log[debug->NextMsg].length;

   return length;
}


/**
 * Verify that source, type, and severity are valid enums.
 *
 * The 'caller' param is used for handling values available
 * only in glDebugMessageInsert or glDebugMessageControl
 */
static GLboolean
validate_params(struct gl_context *ctx, unsigned caller,
                const char *callerstr, GLenum source, GLenum type,
                GLenum severity)
{
#define INSERT 1
#define CONTROL 2
   switch(source) {
   case GL_DEBUG_SOURCE_APPLICATION_ARB:
   case GL_DEBUG_SOURCE_THIRD_PARTY_ARB:
      break;
   case GL_DEBUG_SOURCE_API_ARB:
   case GL_DEBUG_SOURCE_SHADER_COMPILER_ARB:
   case GL_DEBUG_SOURCE_WINDOW_SYSTEM_ARB:
   case GL_DEBUG_SOURCE_OTHER_ARB:
      if (caller != INSERT)
         break;
      else
         goto error;
   case GL_DONT_CARE:
      if (caller == CONTROL)
         break;
      else
         goto error;
   default:
      goto error;
   }

   switch(type) {
   case GL_DEBUG_TYPE_ERROR_ARB:
   case GL_DEBUG_TYPE_DEPRECATED_BEHAVIOR_ARB:
   case GL_DEBUG_TYPE_UNDEFINED_BEHAVIOR_ARB:
   case GL_DEBUG_TYPE_PERFORMANCE_ARB:
   case GL_DEBUG_TYPE_PORTABILITY_ARB:
   case GL_DEBUG_TYPE_OTHER_ARB:
   case GL_DEBUG_TYPE_MARKER:
      break;
   case GL_DEBUG_TYPE_PUSH_GROUP:
   case GL_DEBUG_TYPE_POP_GROUP:
   case GL_DONT_CARE:
      if (caller == CONTROL)
         break;
      else
         goto error;
   default:
      goto error;
   }

   switch(severity) {
   case GL_DEBUG_SEVERITY_HIGH_ARB:
   case GL_DEBUG_SEVERITY_MEDIUM_ARB:
   case GL_DEBUG_SEVERITY_LOW_ARB:
   case GL_DEBUG_SEVERITY_NOTIFICATION:
      break;
   case GL_DONT_CARE:
      if (caller == CONTROL)
         break;
      else
         goto error;
   default:
      goto error;
   }
   return GL_TRUE;

error:
   _mesa_error(ctx, GL_INVALID_ENUM, "bad values passed to %s"
               "(source=0x%x, type=0x%x, severity=0x%x)", callerstr,
               source, type, severity);

   return GL_FALSE;
}


/**
 * Set the state of all message IDs found in the given intersection of
 * 'source', 'type', and 'severity'.  The _COUNT enum can be used for
 * GL_DONT_CARE (include all messages in the class).
 *
 * This requires both setting the state of all previously seen message
 * IDs in the hash table, and setting the default state for all
 * applicable combinations of source/type/severity, so that all the
 * yet-unknown message IDs that may be used in the future will be
 * impacted as if they were already known.
 */
static void
control_messages(struct gl_context *ctx,
                 enum mesa_debug_source source,
                 enum mesa_debug_type type,
                 enum mesa_debug_severity severity,
                 GLboolean enabled)
{
   struct gl_debug_state *debug = _mesa_get_debug_state(ctx);
   int s, t, sev, smax, tmax, sevmax;
   const GLint gstack = debug ? debug->GroupStackDepth : 0;

   if (!debug)
      return;

   if (source == MESA_DEBUG_SOURCE_COUNT) {
      source = 0;
      smax = MESA_DEBUG_SOURCE_COUNT;
   } else {
      smax = source+1;
   }

   if (type == MESA_DEBUG_TYPE_COUNT) {
      type = 0;
      tmax = MESA_DEBUG_TYPE_COUNT;
   } else {
      tmax = type+1;
   }

   if (severity == MESA_DEBUG_SEVERITY_COUNT) {
      severity = 0;
      sevmax = MESA_DEBUG_SEVERITY_COUNT;
   } else {
      sevmax = severity+1;
   }

   for (sev = severity; sev < sevmax; sev++) {
      for (s = source; s < smax; s++) {
         for (t = type; t < tmax; t++) {
            struct simple_node *node;
            struct gl_debug_severity *entry;

            /* change the default for IDs we've never seen before. */
            debug->Defaults[gstack][sev][s][t] = enabled;

            /* Now change the state of IDs we *have* seen... */
            foreach(node, &debug->Namespaces[gstack][s][t].Severity[sev]) {
               entry = (struct gl_debug_severity *)node;
               set_message_state(ctx, s, t, entry->ID, enabled);
            }
         }
      }
   }
}


/**
 * Debugging-message namespaces with the source APPLICATION or THIRD_PARTY
 * require special handling, since the IDs in them are controlled by clients,
 * not the OpenGL implementation.
 *
 * 'count' is the length of the array 'ids'. If 'count' is nonzero, all
 * the given IDs in the namespace defined by 'esource' and 'etype'
 * will be affected.
 *
 * If 'count' is zero, this sets the state of all IDs that match
 * the combination of 'esource', 'etype', and 'eseverity'.
 */
static void
control_app_messages(struct gl_context *ctx, GLenum esource, GLenum etype,
                     GLenum eseverity, GLsizei count, const GLuint *ids,
                     GLboolean enabled)
{
   GLsizei i;
   enum mesa_debug_source source = gl_enum_to_debug_source(esource);
   enum mesa_debug_type type = gl_enum_to_debug_type(etype);
   enum mesa_debug_severity severity = gl_enum_to_debug_severity(eseverity);

   for (i = 0; i < count; i++)
      set_message_state(ctx, source, type, ids[i], enabled);

   if (count)
      return;

   control_messages(ctx, source, type, severity, enabled);
}


/**
 * This is a generic message insert function.
 * Validation of source, type and severity parameters should be done
 * before calling this funtion.
 */
static void
message_insert(GLenum source, GLenum type, GLuint id,
               GLenum severity, GLint length, const GLchar *buf,
               const char *callerstr)
{
   GET_CURRENT_CONTEXT(ctx);

   if (length < 0)
      length = strlen(buf);

   if (length >= MAX_DEBUG_MESSAGE_LENGTH) {
      _mesa_error(ctx, GL_INVALID_VALUE,
                 "%s(length=%d, which is not less than "
                 "GL_MAX_DEBUG_MESSAGE_LENGTH=%d)", callerstr, length,
                 MAX_DEBUG_MESSAGE_LENGTH);
      return;
   }

   log_msg(ctx,
           gl_enum_to_debug_source(source),
           gl_enum_to_debug_type(type), id,
           gl_enum_to_debug_severity(severity), length, buf);
}


static void
do_nothing(GLuint key, void *data, void *userData)
{
}


/**
 * Free context state pertaining to error/debug state for the given stack
 * depth.
 */
static void
free_errors_data(struct gl_context *ctx, GLint gstack)
{
   struct gl_debug_state *debug = ctx->Debug;
   enum mesa_debug_type t;
   enum mesa_debug_source s;
   enum mesa_debug_severity sev;

   assert(debug);

   /* Tear down state for filtering debug messages. */
   for (s = 0; s < MESA_DEBUG_SOURCE_COUNT; s++) {
      for (t = 0; t < MESA_DEBUG_TYPE_COUNT; t++) {
         _mesa_HashDeleteAll(debug->Namespaces[gstack][s][t].IDs,
                             do_nothing, NULL);
         _mesa_DeleteHashTable(debug->Namespaces[gstack][s][t].IDs);
         for (sev = 0; sev < MESA_DEBUG_SEVERITY_COUNT; sev++) {
            struct simple_node *node, *tmp;
            struct gl_debug_severity *entry;

            foreach_s(node, tmp,
                      &debug->Namespaces[gstack][s][t].Severity[sev]) {
               entry = (struct gl_debug_severity *)node;
               free(entry);
            }
         }
      }
   }
}


void GLAPIENTRY
_mesa_DebugMessageInsert(GLenum source, GLenum type, GLuint id,
                         GLenum severity, GLint length,
                         const GLchar *buf)
{
   const char *callerstr = "glDebugMessageInsert";

   GET_CURRENT_CONTEXT(ctx);

   if (!validate_params(ctx, INSERT, callerstr, source, type, severity))
      return; /* GL_INVALID_ENUM */

   message_insert(source, type, id, severity, length, buf, callerstr);
}


GLuint GLAPIENTRY
_mesa_GetDebugMessageLog(GLuint count, GLsizei logSize, GLenum *sources,
                         GLenum *types, GLenum *ids, GLenum *severities,
                         GLsizei *lengths, GLchar *messageLog)
{
   GET_CURRENT_CONTEXT(ctx);
   GLuint ret;

   if (!messageLog)
      logSize = 0;

   if (logSize < 0) {
      _mesa_error(ctx, GL_INVALID_VALUE,
                  "glGetDebugMessageLog(logSize=%d : logSize must not be"
                  " negative)", logSize);
      return 0;
   }

   for (ret = 0; ret < count; ret++) {
      GLsizei written = get_msg(ctx, sources, types, ids, severities,
                                logSize, messageLog);
      if (!written)
         break;

      if (messageLog) {
         messageLog += written;
         logSize -= written;
      }
      if (lengths) {
         *lengths = written;
         lengths++;
      }

      if (severities)
         severities++;
      if (sources)
         sources++;
      if (types)
         types++;
      if (ids)
         ids++;
   }

   return ret;
}


void GLAPIENTRY
_mesa_DebugMessageControl(GLenum gl_source, GLenum gl_type,
                          GLenum gl_severity, GLsizei count,
                          const GLuint *ids, GLboolean enabled)
{
   const char *callerstr = "glDebugMessageControl";

   GET_CURRENT_CONTEXT(ctx);

   if (count < 0) {
      _mesa_error(ctx, GL_INVALID_VALUE,
                  "%s(count=%d : count must not be negative)", callerstr,
                  count);
      return;
   }

   if (!validate_params(ctx, CONTROL, callerstr, gl_source, gl_type,
                        gl_severity))
      return; /* GL_INVALID_ENUM */

   if (count && (gl_severity != GL_DONT_CARE || gl_type == GL_DONT_CARE
                 || gl_source == GL_DONT_CARE)) {
      _mesa_error(ctx, GL_INVALID_OPERATION,
                  "%s(When passing an array of ids, severity must be"
         " GL_DONT_CARE, and source and type must not be GL_DONT_CARE.",
                  callerstr);
      return;
   }

   control_app_messages(ctx, gl_source, gl_type, gl_severity,
                        count, ids, enabled);
}


void GLAPIENTRY
_mesa_DebugMessageCallback(GLDEBUGPROC callback, const void *userParam)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_debug_state *debug = _mesa_get_debug_state(ctx);
   if (debug) {
      debug->Callback = callback;
      debug->CallbackData = userParam;
   }
}


void GLAPIENTRY
_mesa_PushDebugGroup(GLenum source, GLuint id, GLsizei length,
                     const GLchar *message)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_debug_state *debug = _mesa_get_debug_state(ctx);
   const char *callerstr = "glPushDebugGroup";
   int s, t, sev;
   GLint prevStackDepth;
   GLint currStackDepth;
   struct gl_debug_msg *emptySlot;

   if (!debug)
      return;

   if (debug->GroupStackDepth >= MAX_DEBUG_GROUP_STACK_DEPTH-1) {
      _mesa_error(ctx, GL_STACK_OVERFLOW, "%s", callerstr);
      return;
   }

   switch(source) {
   case GL_DEBUG_SOURCE_APPLICATION:
   case GL_DEBUG_SOURCE_THIRD_PARTY:
      break;
   default:
      _mesa_error(ctx, GL_INVALID_ENUM, "bad value passed to %s"
                  "(source=0x%x)", callerstr, source);
      return;
   }

   message_insert(source, GL_DEBUG_TYPE_PUSH_GROUP, id,
                  GL_DEBUG_SEVERITY_NOTIFICATION, length,
                  message, callerstr);

   prevStackDepth = debug->GroupStackDepth;
   debug->GroupStackDepth++;
   currStackDepth = debug->GroupStackDepth;

   /* pop reuses the message details from push so we store this */
   if (length < 0)
      length = strlen(message);
   emptySlot = &debug->DebugGroupMsgs[debug->GroupStackDepth];
   store_message_details(emptySlot, gl_enum_to_debug_source(source),
                         gl_enum_to_debug_type(GL_DEBUG_TYPE_PUSH_GROUP),
                         id,
                   gl_enum_to_debug_severity(GL_DEBUG_SEVERITY_NOTIFICATION),
                         length, message);

   /* inherit the control volume of the debug group previously residing on
    * the top of the debug group stack
    */
   for (s = 0; s < MESA_DEBUG_SOURCE_COUNT; s++) {
      for (t = 0; t < MESA_DEBUG_TYPE_COUNT; t++) {
         /* copy id settings */
         debug->Namespaces[currStackDepth][s][t].IDs =
            _mesa_HashClone(debug->Namespaces[prevStackDepth][s][t].IDs);

         for (sev = 0; sev < MESA_DEBUG_SEVERITY_COUNT; sev++) {
            struct gl_debug_severity *entry, *prevEntry;
            struct simple_node *node;

            /* copy default settings for unknown ids */
            debug->Defaults[currStackDepth][sev][s][t] =
               debug->Defaults[prevStackDepth][sev][s][t];

            /* copy known id severity settings */
            make_empty_list(&debug->Namespaces[currStackDepth][s][t].Severity[sev]);
            foreach(node, &debug->Namespaces[prevStackDepth][s][t].Severity[sev]) {
               prevEntry = (struct gl_debug_severity *)node;
               entry = malloc(sizeof *entry);
               if (!entry)
                  return;

               entry->ID = prevEntry->ID;
               insert_at_tail(&debug->Namespaces[currStackDepth][s][t].Severity[sev], &entry->link);
            }
         }
      }
   }
}


void GLAPIENTRY
_mesa_PopDebugGroup(void)
{
   GET_CURRENT_CONTEXT(ctx);
   struct gl_debug_state *debug = _mesa_get_debug_state(ctx);
   const char *callerstr = "glPopDebugGroup";
   struct gl_debug_msg *gdmessage;
   GLint prevStackDepth;

   if (!debug)
      return;

   if (debug->GroupStackDepth <= 0) {
      _mesa_error(ctx, GL_STACK_UNDERFLOW, "%s", callerstr);
      return;
   }

   prevStackDepth = debug->GroupStackDepth;
   debug->GroupStackDepth--;

   gdmessage = &debug->DebugGroupMsgs[prevStackDepth];
   /* using log_msg() directly here as verification of parameters
    * already done in push
    */
   log_msg(ctx, gdmessage->source,
           gl_enum_to_debug_type(GL_DEBUG_TYPE_POP_GROUP),
           gdmessage->id,
           gl_enum_to_debug_severity(GL_DEBUG_SEVERITY_NOTIFICATION),
           gdmessage->length, gdmessage->message);

   if (gdmessage->message != (char*)out_of_memory)
      free(gdmessage->message);
   gdmessage->message = NULL;
   gdmessage->length = 0;

   /* free popped debug group data */
   free_errors_data(ctx, prevStackDepth);
}


void
_mesa_init_errors(struct gl_context *ctx)
{
   /* no-op */
}


/**
 * Loop through debug group stack tearing down states for
 * filtering debug messages.  Then free debug output state.
 */
void
_mesa_free_errors_data(struct gl_context *ctx)
{
   if (ctx->Debug) {
      GLint i;

      for (i = 0; i <= ctx->Debug->GroupStackDepth; i++) {
         free_errors_data(ctx, i);
      }
      free(ctx->Debug);
      /* set to NULL just in case it is used before context is completely gone. */
      ctx->Debug = NULL;
   }
}


/**********************************************************************/
/** \name Diagnostics */
/*@{*/

static void
output_if_debug(const char *prefixString, const char *outputString,
                GLboolean newline)
{
   static int debug = -1;
   static FILE *fout = NULL;

   /* Init the local 'debug' var once.
    * Note: the _mesa_init_debug() function should have been called
    * by now so MESA_DEBUG_FLAGS will be initialized.
    */
   if (debug == -1) {
      /* If MESA_LOG_FILE env var is set, log Mesa errors, warnings,
       * etc to the named file.  Otherwise, output to stderr.
       */
      const char *logFile = _mesa_getenv("MESA_LOG_FILE");
      if (logFile)
         fout = fopen(logFile, "w");
      if (!fout)
         fout = stderr;
#ifdef DEBUG
      /* in debug builds, print messages unless MESA_DEBUG="silent" */
      if (MESA_DEBUG_FLAGS & DEBUG_SILENT)
         debug = 0;
      else
         debug = 1;
#else
      /* in release builds, be silent unless MESA_DEBUG is set */
      debug = _mesa_getenv("MESA_DEBUG") != NULL;
#endif
   }

   /* Now only print the string if we're required to do so. */
   if (debug) {
      fprintf(fout, "%s: %s", prefixString, outputString);
      if (newline)
         fprintf(fout, "\n");
      fflush(fout);

#if defined(_WIN32) && !defined(_WIN32_WCE)
      /* stderr from windows applications without console is not usually 
       * visible, so communicate with the debugger instead */ 
      {
         char buf[4096];
         _mesa_snprintf(buf, sizeof(buf), "%s: %s%s", prefixString, outputString, newline ? "\n" : "");
         OutputDebugStringA(buf);
      }
#endif
   }
}


/**
 * When a new type of error is recorded, print a message describing
 * previous errors which were accumulated.
 */
static void
flush_delayed_errors( struct gl_context *ctx )
{
   char s[MAX_DEBUG_MESSAGE_LENGTH];

   if (ctx->ErrorDebugCount) {
      _mesa_snprintf(s, MAX_DEBUG_MESSAGE_LENGTH, "%d similar %s errors", 
                     ctx->ErrorDebugCount,
                     _mesa_lookup_enum_by_nr(ctx->ErrorValue));

      output_if_debug("Mesa", s, GL_TRUE);

      ctx->ErrorDebugCount = 0;
   }
}


/**
 * Report a warning (a recoverable error condition) to stderr if
 * either DEBUG is defined or the MESA_DEBUG env var is set.
 *
 * \param ctx GL context.
 * \param fmtString printf()-like format string.
 */
void
_mesa_warning( struct gl_context *ctx, const char *fmtString, ... )
{
   char str[MAX_DEBUG_MESSAGE_LENGTH];
   va_list args;
   va_start( args, fmtString );  
   (void) _mesa_vsnprintf( str, MAX_DEBUG_MESSAGE_LENGTH, fmtString, args );
   va_end( args );
   
   if (ctx)
      flush_delayed_errors( ctx );

   output_if_debug("Mesa warning", str, GL_TRUE);
}


/**
 * Report an internal implementation problem.
 * Prints the message to stderr via fprintf().
 *
 * \param ctx GL context.
 * \param fmtString problem description string.
 */
void
_mesa_problem( const struct gl_context *ctx, const char *fmtString, ... )
{
   va_list args;
   char str[MAX_DEBUG_MESSAGE_LENGTH];
   static int numCalls = 0;

   (void) ctx;

   if (numCalls < 50) {
      numCalls++;

      va_start( args, fmtString );  
      _mesa_vsnprintf( str, MAX_DEBUG_MESSAGE_LENGTH, fmtString, args );
      va_end( args );
      fprintf(stderr, "Mesa %s implementation error: %s\n",
              PACKAGE_VERSION, str);
      fprintf(stderr, "Please report at " PACKAGE_BUGREPORT "\n");
   }
}


static GLboolean
should_output(struct gl_context *ctx, GLenum error, const char *fmtString)
{
   static GLint debug = -1;

   /* Check debug environment variable only once:
    */
   if (debug == -1) {
      const char *debugEnv = _mesa_getenv("MESA_DEBUG");

#ifdef DEBUG
      if (debugEnv && strstr(debugEnv, "silent"))
         debug = GL_FALSE;
      else
         debug = GL_TRUE;
#else
      if (debugEnv)
         debug = GL_TRUE;
      else
         debug = GL_FALSE;
#endif
   }

   if (debug) {
      if (ctx->ErrorValue != error ||
          ctx->ErrorDebugFmtString != fmtString) {
         flush_delayed_errors( ctx );
         ctx->ErrorDebugFmtString = fmtString;
         ctx->ErrorDebugCount = 0;
         return GL_TRUE;
      }
      ctx->ErrorDebugCount++;
   }
   return GL_FALSE;
}


void
_mesa_gl_debug(struct gl_context *ctx,
               GLuint *id,
               enum mesa_debug_type type,
               enum mesa_debug_severity severity,
               const char *fmtString, ...)
{
   char s[MAX_DEBUG_MESSAGE_LENGTH];
   int len;
   va_list args;

   debug_get_id(id);

   va_start(args, fmtString);
   len = _mesa_vsnprintf(s, MAX_DEBUG_MESSAGE_LENGTH, fmtString, args);
   va_end(args);

   log_msg(ctx, MESA_DEBUG_SOURCE_API, type, *id, severity, len, s);
}


/**
 * Record an OpenGL state error.  These usually occur when the user
 * passes invalid parameters to a GL function.
 *
 * If debugging is enabled (either at compile-time via the DEBUG macro, or
 * run-time via the MESA_DEBUG environment variable), report the error with
 * _mesa_debug().
 * 
 * \param ctx the GL context.
 * \param error the error value.
 * \param fmtString printf() style format string, followed by optional args
 */
void
_mesa_error( struct gl_context *ctx, GLenum error, const char *fmtString, ... )
{
   GLboolean do_output, do_log;
   /* Ideally this would be set up by the caller, so that we had proper IDs
    * per different message.
    */
   static GLuint error_msg_id = 0;

   debug_get_id(&error_msg_id);

   do_output = should_output(ctx, error, fmtString);
   do_log = should_log(ctx,
                       MESA_DEBUG_SOURCE_API,
                       MESA_DEBUG_TYPE_ERROR,
                       error_msg_id,
                       MESA_DEBUG_SEVERITY_HIGH);

   if (do_output || do_log) {
      char s[MAX_DEBUG_MESSAGE_LENGTH], s2[MAX_DEBUG_MESSAGE_LENGTH];
      int len;
      va_list args;

      va_start(args, fmtString);
      len = _mesa_vsnprintf(s, MAX_DEBUG_MESSAGE_LENGTH, fmtString, args);
      va_end(args);

      if (len >= MAX_DEBUG_MESSAGE_LENGTH) {
         /* Too long error message. Whoever calls _mesa_error should use
          * shorter strings.
          */
         ASSERT(0);
         return;
      }

      len = _mesa_snprintf(s2, MAX_DEBUG_MESSAGE_LENGTH, "%s in %s",
                           _mesa_lookup_enum_by_nr(error), s);
      if (len >= MAX_DEBUG_MESSAGE_LENGTH) {
         /* Same as above. */
         ASSERT(0);
         return;
      }

      /* Print the error to stderr if needed. */
      if (do_output) {
         output_if_debug("Mesa: User error", s2, GL_TRUE);
      }

      /* Log the error via ARB_debug_output if needed.*/
      if (do_log) {
         log_msg(ctx, MESA_DEBUG_SOURCE_API, MESA_DEBUG_TYPE_ERROR,
                 error_msg_id, MESA_DEBUG_SEVERITY_HIGH, len, s2);
      }
   }

   /* Set the GL context error state for glGetError. */
   _mesa_record_error(ctx, error);
}


/**
 * Report debug information.  Print error message to stderr via fprintf().
 * No-op if DEBUG mode not enabled.
 * 
 * \param ctx GL context.
 * \param fmtString printf()-style format string, followed by optional args.
 */
void
_mesa_debug( const struct gl_context *ctx, const char *fmtString, ... )
{
#ifdef DEBUG
   char s[MAX_DEBUG_MESSAGE_LENGTH];
   va_list args;
   va_start(args, fmtString);
   _mesa_vsnprintf(s, MAX_DEBUG_MESSAGE_LENGTH, fmtString, args);
   va_end(args);
   output_if_debug("Mesa", s, GL_FALSE);
#endif /* DEBUG */
   (void) ctx;
   (void) fmtString;
}


/**
 * Report debug information from the shader compiler via GL_ARB_debug_output.
 *
 * \param ctx GL context.
 * \param type The namespace to which this message belongs.
 * \param id The message ID within the given namespace.
 * \param msg The message to output. Need not be null-terminated.
 * \param len The length of 'msg'. If negative, 'msg' must be null-terminated.
 */
void
_mesa_shader_debug( struct gl_context *ctx, GLenum type, GLuint *id,
                    const char *msg, int len )
{
   enum mesa_debug_source source = MESA_DEBUG_SOURCE_SHADER_COMPILER;
   enum mesa_debug_severity severity = MESA_DEBUG_SEVERITY_HIGH;

   debug_get_id(id);

   if (len < 0)
      len = strlen(msg);

   /* Truncate the message if necessary. */
   if (len >= MAX_DEBUG_MESSAGE_LENGTH)
      len = MAX_DEBUG_MESSAGE_LENGTH - 1;

   log_msg(ctx, source, type, *id, severity, len, msg);
}

/*@}*/
