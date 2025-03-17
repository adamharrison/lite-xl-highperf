#if _WIN32
  #include <windows.h>
#else
  #include <pthread.h>
  #include <unistd.h>
  #define MAX_PATH PATH_MAX
#endif
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>

#ifdef HIGHPERFSESARCH_STANDALONE
  #include <lua.h>
  #include <lauxlib.h>
  #include <lualib.h>
#else
  #define LITE_XL_PLUGIN_ENTRYPOINT
  #include <lite_xl_plugin_api.h>
#endif

int imax(int a, int b) { return a > b ? a : b; }
int imin(int a, int b) { return a < b ? a : b; }

typedef struct {
  #if _WIN32
    HANDLE thread;
    void* (*func)(void*);
    void* data;
  #else
    pthread_t thread;
  #endif
} thread_t;

typedef struct {
  #if _WIN32
    HANDLE mutex;
  #else
    pthread_mutex_t mutex;
  #endif
} mutex_t;

static mutex_t* new_mutex() {
  mutex_t* mutex = malloc(sizeof(mutex_t));
  #if _WIN32
    mutex->mutex = CreateMutex(NULL, FALSE, NULL);
  #else
    pthread_mutex_init(&mutex->mutex, NULL);
  #endif
  return mutex;
}

static void free_mutex(mutex_t* mutex) {
  #if _WIN32
    CloseHandle(mutex->mutex);
  #else
    pthread_mutex_destroy(&mutex->mutex);
  #endif
  free(mutex);
}

static void lock_mutex(mutex_t* mutex) {
  #if _WIN32
    WaitForSingleObject(mutex->mutex, INFINITE);
  #else
    pthread_mutex_lock(&mutex->mutex);
  #endif
}

static void unlock_mutex(mutex_t* mutex) {
  #if _WIN32
    ReleaseMutex(mutex->mutex);
  #else
    pthread_mutex_unlock(&mutex->mutex);
  #endif
}

static thread_t* create_thread(void* (*func)(void*), void* data) {
  thread_t* thread = malloc(sizeof(thread_t));
  #if _WIN32
    thread->func = func;
    thread->data = data;
    thread->thread = CreateThread(NULL, 0, windows_thread_callback, thread, 0, NULL);
  #else
    pthread_create(&thread->thread, NULL, func, data);
  #endif
  return thread;
}

static void* join_thread(thread_t* thread) {
  void* retval;
  #if _WIN32
    WaitForSingleObject(thread->thread, INFINITE);
  #else
    pthread_join(thread->thread, &retval);
  #endif
  free(thread);
  return retval;
}

static int msleep(int miliseconds) {
  #if _WIN32
    Sleep(miliseconds);
  #else
    usleep(miliseconds * 1000);
  #endif
}

#define SEARCH_CONTEXT_LENGTH 80
#define MAX_SEARCH_HIT_LENGTH 256
#define MAX_SEARCH_STRING MAX_SEARCH_HIT_LENGTH
#define CHUNK_SIZE 16384

typedef struct {
  int found_idx;
  int line;
  int col;
  char text[MAX_SEARCH_HIT_LENGTH];
} match_entry_t;

typedef struct {
  char path[MAX_PATH+1];
} file_entry_t;

typedef enum {
  SEARCH_PLAIN,
  SEARCH_INSENSITIVE
} search_type_e;

typedef struct {
  int callback_function;
  mutex_t* mutex;
  thread_t** threads;
  int threads_complete;
  int thread_count;
  int entry_start;
  file_entry_t* entries;
  int entry_capacity;
  int entry_count;

  match_entry_t* matches;
  int match_count;
  int match_capacity;
  
  search_type_e type;
  char search_string[MAX_SEARCH_STRING];
  volatile int is_done;
} search_state_t;

static void* thread_callback(void* data) {
  search_state_t* state = data;
  char chunk[CHUNK_SIZE];
  int search_length = strlen(state->search_string);
  while (state->entry_start < state->entry_count || !state->is_done) {
    int targeted_entry = -1;
    lock_mutex(state->mutex);
    if (state->entry_start < state->entry_count)
      targeted_entry = state->entry_start++;
    unlock_mutex(state->mutex);
    if (targeted_entry != -1) {
      FILE* file = fopen(state->entries[targeted_entry].path, "rb");
      if (file) {
        int line = 1;
        int col = 1;
        int offset = 0;
        while (1) {
          int length_to_read = sizeof(chunk) - offset;
          int length_read = fread(&chunk[offset], 1, length_to_read, file);
          if (length_read <= 0)
            break;
          int total_length = offset + length_read;
          if (total_length < search_length)
            break;
          int max_length_to_search = imax(total_length - search_length - SEARCH_CONTEXT_LENGTH, search_length);
          for (int i = 0; i < max_length_to_search; ++i, ++col) {
            if (chunk[i] == '\n') {
              line++;
              col = 1;
            }
            int match = 1;
            switch (state->type) {
              case SEARCH_PLAIN:
                if (strncmp(&chunk[i], state->search_string, search_length) != 0) {
                  match = 0;
                }
              break;
              case SEARCH_INSENSITIVE:
                for (int j = 0; j < search_length; ++j) {
                  if (tolower(chunk[i+j]) != state->search_string[j]) {
                      match = 0;
                      break;
                  }
                }
              break;
            }
            if (match) {
              lock_mutex(state->mutex);
                if (state->match_count == state->match_capacity) {
                  state->match_capacity = imax(1, state->match_capacity * 2);
                  state->matches = realloc(state->matches, sizeof(match_entry_t)*state->match_capacity);
                }
                state->matches[state->match_count].found_idx = targeted_entry;
                state->matches[state->match_count].line = line;
                state->matches[state->match_count].col = col - 1;
                int start = imax(i - SEARCH_CONTEXT_LENGTH, 0);
                for (int j = i; j >= start; --j) {
                  if (chunk[j] == '\n') {
                    start = j + 1;
                    break;
                  }
                }
                int max_length = imin(MAX_SEARCH_HIT_LENGTH, length_read - start);
                for (int j = 0; j < max_length; ++j) {
                  if (chunk[start+j] == '\n') {
                    max_length = j;
                    break;
                  }
                }
                strncpy(state->matches[state->match_count].text, &chunk[start], max_length);
                ++state->match_count;
              unlock_mutex(state->mutex);
            }
          }
          if (length_read < length_to_read) {
            break;
          } else {
            memcpy(chunk, &chunk[max_length_to_search], sizeof(chunk) - max_length_to_search);
            offset = sizeof(chunk) - max_length_to_search;
          }
        }
        fclose(file);
      }
    } else {
      msleep(1);
    }
  }
  lock_mutex(state->mutex);
    state->threads_complete += 1;
  unlock_mutex(state->mutex);
}

static int f_search_update(lua_State* L) {
  search_state_t* state = luaL_checkudata(L, 1, "highperfsearch");
  if (state->match_count) {
    lua_rawgeti(L, LUA_REGISTRYINDEX, state->callback_function);
    lock_mutex(state->mutex);
      for (int i = 0; i < state->match_count; ++i) {
        match_entry_t* match = &state->matches[i];
        lua_pushvalue(L, -1);
        lua_pushstring(L, state->entries[match->found_idx].path);
        lua_pushinteger(L, match->line);
        lua_pushinteger(L, match->col);
        lua_pushstring(L, match->text);
        lua_call(L, 4, 0);
      }
      state->match_count = 0;
    unlock_mutex(state->mutex);
    lua_pop(L, 1);
  }
}


static int f_search_init(lua_State* L) {
  int threads = luaL_checkinteger(L, 1);
  search_type_e type;
  const char* search_string = luaL_checkstring(L, 2);
  const char* type_string = luaL_checkstring(L, 3);
  if (strcmp(type_string, "plain") == 0)
    type = SEARCH_PLAIN;
  else if (strcmp(type_string, "insensitive") == 0) {
    type = SEARCH_INSENSITIVE;
  } else
    return luaL_error(L, "invalid search type: %s", type_string);
  luaL_checktype(L, 4, LUA_TFUNCTION);
  int ref = luaL_ref(L, LUA_REGISTRYINDEX);
  search_state_t* state = lua_newuserdata(L, sizeof(search_state_t));
  memset(state, 0, sizeof(search_state_t));
  luaL_setmetatable(L, "highperfsearch");
  state->type = type;
  state->callback_function = ref;
  state->mutex = new_mutex();
  state->thread_count = threads;
  strncpy(state->search_string, search_string, sizeof(state->search_string) - 1);
  if (type == SEARCH_INSENSITIVE) {
    for (int i = 0; i < strlen(state->search_string); ++i)
      state->search_string[i] = tolower(state->search_string[i]);
  }
  state->threads = malloc(sizeof(thread_t*) * threads);
  for (int i = 0; i < threads; ++i)
    state->threads[i] = create_thread(thread_callback, state);
  return 1;
}

static int f_search_find(lua_State* L) {
  search_state_t* state = luaL_checkudata(L, 1, "highperfsearch");
  size_t length;
  const char* path = luaL_checklstring(L, 2, &length);
  if (length > MAX_PATH)
    return luaL_error(L, "path of %s too large", path);
  f_search_update(L);
  lock_mutex(state->mutex);
    if (state->entry_count == state->entry_capacity) {
      state->entry_capacity = imax(1, state->entry_capacity * 2);
      state->entries = realloc(state->entries, sizeof(file_entry_t)*state->entry_capacity);
    }
    strcpy(state->entries[state->entry_count].path, path);
    state->entry_count++;
  unlock_mutex(state->mutex);
  return 0;
}

static int f_search_joink(lua_State* L, int status, lua_KContext ctx) {
  search_state_t* state = luaL_checkudata(L, 1, "highperfsearch");
  if (state->threads_complete == state->thread_count) {
    for (int i = 0; i < state->thread_count; ++i) {
      if (state->threads[i]) {
        join_thread(state->threads[i]);
        state->threads[i] = NULL;
      }
    }
    return 0;
  }
  f_search_update(L);
  lua_pushinteger(L, 0);
  return lua_yieldk(L, 1, 0, f_search_joink);
}

static int f_search_join(lua_State* L) {
  search_state_t* state = luaL_checkudata(L, 1, "highperfsearch");
  state->is_done = 1;
  lua_pushinteger(L, 0);
  return lua_yieldk(L, 1, 0, f_search_joink);
}

static int f_search_gc(lua_State* L) {
  search_state_t* state = luaL_checkudata(L, 1, "highperfsearch");
  f_search_join(L);
  free(state->threads);
  free_mutex(state->mutex);
  if (state->matches)
    free(state->matches);
  if (state->entries)
    free(state->entries);
  return 0;
}

// Core functions, `request` is the primary function, and is stateless (minus the ssl config), and makes raw requests.
static const luaL_Reg search_api[] = {
  { "__gc",          f_search_gc         },
  { "init",          f_search_init       },
  { "find",          f_search_find       },
  { "join",          f_search_join       },
  { NULL,            NULL                }
};

#ifndef HIGHPERF_VERSION
  #define HIGHPERF_VERSION "unknown"
#endif


#ifndef HIGHPERFSEARCH_STANDALONE
int luaopen_lite_xl_hpprojectsearch(lua_State* L, void* XL) {
  lite_xl_plugin_init(XL);
#else
int luaopen_hpprojectsearch(lua_State* L) {
#endif
  luaL_newmetatable(L, "highperfsearch");
  luaL_setfuncs(L, search_api, 0);
  lua_pushliteral(L, HIGHPERF_VERSION);
  lua_setfield(L, -2, "version");
  lua_pushvalue(L, -1);
  lua_setfield(L, -2, "__index");
  lua_pushvalue(L, -1);
  return 1;
}
