//
// Copyright 2015, Google Inc.
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
// notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
// copyright notice, this list of conditions and the following disclaimer
// in the documentation and/or other materials provided with the
// distribution.
//     * Neither the name of Google Inc. nor the names of its
// contributors may be used to endorse or promote products derived from
// this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//

#include "src/core/lib/transport/service_config.h"

#include <string.h>

#include <grpc/impl/codegen/grpc_types.h>
#include <grpc/support/alloc.h>
#include <grpc/support/log.h>
#include <grpc/support/string_util.h>

#include "src/core/lib/json/json.h"
#include "src/core/lib/support/string.h"
#include "src/core/lib/transport/mdstr_hash_table.h"

// The main purpose of the code here is to parse the service config in
// JSON form, which will look like this:
//
// {
//   "loadBalancingPolicy": "string",  // optional
//   "methodConfig": [  // array of one or more method_config objects
//     {
//       "name": [  // array of one or more name objects
//         {
//           "service": "string",  // required
//           "method": "string",  // optional
//         }
//       ],
//       // remaining fields are optional.
//       // see https://developers.google.com/protocol-buffers/docs/proto3#json
//       // for format details.
//       "waitForReady": bool,
//       "timeout": "duration_string",
//       "maxRequestMessageBytes": "int64_string",
//       "maxResponseMessageBytes": "int64_string",
//     }
//   ]
// }

struct grpc_service_config {
  char* json_string;  // Underlying storage for json_tree.
  grpc_json* json_tree;
};

grpc_service_config* grpc_service_config_create(const char* json_string) {
  grpc_service_config* service_config = gpr_malloc(sizeof(*service_config));
  service_config->json_string = gpr_strdup(json_string);
  service_config->json_tree =
      grpc_json_parse_string(service_config->json_string);
  if (service_config->json_tree == NULL) {
    gpr_log(GPR_INFO, "failed to parse JSON for service config");
    gpr_free(service_config->json_string);
    gpr_free(service_config);
    return NULL;
  }
  return service_config;
}

void grpc_service_config_destroy(grpc_service_config* service_config) {
  grpc_json_destroy(service_config->json_tree);
  gpr_free(service_config->json_string);
  gpr_free(service_config);
}

const char* grpc_service_config_get_lb_policy_name(
    const grpc_service_config* service_config) {
  const grpc_json* json = service_config->json_tree;
  if (json->type != GRPC_JSON_OBJECT || json->key != NULL) return NULL;
  const char* lb_policy_name = NULL;
  for (grpc_json* field = json->child; field != NULL; field = field->next) {
    if (field->key == NULL) return NULL;
    if (strcmp(field->key, "loadBalancingPolicy") == 0) {
      if (lb_policy_name != NULL) return NULL;  // Duplicate.
      if (field->type != GRPC_JSON_STRING) return NULL;
      lb_policy_name = field->value;
    }
  }
  return lb_policy_name;
}

// Returns the number of names specified in the method config \a json.
static size_t count_names_in_method_config_json(grpc_json* json) {
  size_t num_names = 0;
  for (grpc_json* field = json->child; field != NULL; field = field->next) {
    if (field->key != NULL && strcmp(field->key, "name") == 0) ++num_names;
  }
  return num_names;
}

// Returns a path string for the JSON name object specified by \a json.
// Returns NULL on error.  Caller takes ownership of result.
static char* parse_json_method_name(grpc_json* json) {
  if (json->type != GRPC_JSON_OBJECT) return NULL;
  const char* service_name = NULL;
  const char* method_name = NULL;
  for (grpc_json* child = json->child; child != NULL; child = child->next) {
    if (child->key == NULL) return NULL;
    if (child->type != GRPC_JSON_STRING) return NULL;
    if (strcmp(child->key, "service") == 0) {
      if (service_name != NULL) return NULL;  // Duplicate.
      if (child->value == NULL) return NULL;
      service_name = child->value;
    } else if (strcmp(child->key, "method") == 0) {
      if (method_name != NULL) return NULL;  // Duplicate.
      if (child->value == NULL) return NULL;
      method_name = child->value;
    }
  }
  if (service_name == NULL) return NULL;  // Required field.
  char* path;
  gpr_asprintf(&path, "/%s/%s", service_name,
               method_name == NULL ? "*" : method_name);
  return path;
}

// Parses the method config from \a json.  Adds an entry to \a entries for
// each name found, incrementing \a idx for each entry added.
// Returns false on error.
static bool parse_json_method_config(
    grpc_json* json, void* (*create_value)(const grpc_json* method_config_json),
    const grpc_mdstr_hash_table_vtable* vtable,
    grpc_mdstr_hash_table_entry* entries, size_t* idx) {
  // Construct value.
  void* method_config = create_value(json);
  if (method_config == NULL) return false;
  // Construct list of paths.
  bool success = false;
  gpr_strvec paths;
  gpr_strvec_init(&paths);
  for (grpc_json* child = json->child; child != NULL; child = child->next) {
    if (child->key == NULL) continue;
    if (strcmp(child->key, "name") == 0) {
      if (child->type != GRPC_JSON_ARRAY) goto done;
      for (grpc_json* name = child->child; name != NULL; name = name->next) {
        char* path = parse_json_method_name(name);
        gpr_strvec_add(&paths, path);
      }
    }
  }
  if (paths.count == 0) goto done;  // No names specified.
  // Add entry for each path.
  for (size_t i = 0; i < paths.count; ++i) {
    entries[*idx].key = grpc_mdstr_from_string(paths.strs[i]);
    entries[*idx].value = vtable->copy_value(method_config);
    entries[*idx].vtable = vtable;
    ++*idx;
  }
  success = true;
done:
  vtable->destroy_value(method_config);
  gpr_strvec_destroy(&paths);
  return success;
}

grpc_mdstr_hash_table* grpc_service_config_create_method_config_table(
    const grpc_service_config* service_config,
    void* (*create_value)(const grpc_json* method_config_json),
    const grpc_mdstr_hash_table_vtable* vtable) {
  const grpc_json* json = service_config->json_tree;
  // Traverse parsed JSON tree.
  if (json->type != GRPC_JSON_OBJECT || json->key != NULL) return NULL;
  size_t num_entries = 0;
  grpc_mdstr_hash_table_entry* entries = NULL;
  for (grpc_json* field = json->child; field != NULL; field = field->next) {
    if (field->key == NULL) return NULL;
    if (strcmp(field->key, "methodConfig") == 0) {
      if (entries != NULL) return NULL;  // Duplicate.
      if (field->type != GRPC_JSON_ARRAY) return NULL;
      // Find number of entries.
      for (grpc_json* method = field->child; method != NULL;
           method = method->next) {
        num_entries += count_names_in_method_config_json(method);
      }
      // Populate method config table entries.
      entries = gpr_malloc(num_entries * sizeof(grpc_mdstr_hash_table_entry));
      size_t idx = 0;
      for (grpc_json* method = field->child; method != NULL;
           method = method->next) {
        if (!parse_json_method_config(method, create_value, vtable, entries,
                                      &idx)) {
          return NULL;
        }
      }
      GPR_ASSERT(idx == num_entries);
    }
  }
  // Instantiate method config table.
  grpc_mdstr_hash_table* method_config_table = NULL;
  if (entries != NULL) {
    method_config_table = grpc_mdstr_hash_table_create(num_entries, entries);
    // Clean up.
    for (size_t i = 0; i < num_entries; ++i) {
      GRPC_MDSTR_UNREF(entries[i].key);
      vtable->destroy_value(entries[i].value);
    }
    gpr_free(entries);
  }
  return method_config_table;
}

void* grpc_method_config_table_get(const grpc_mdstr_hash_table* table,
                                   const grpc_mdstr* path) {
  void* value = grpc_mdstr_hash_table_get(table, path);
  // If we didn't find a match for the path, try looking for a wildcard
  // entry (i.e., change "/service/method" to "/service/*").
  if (value == NULL) {
    const char* path_str = grpc_mdstr_as_c_string(path);
    const char* sep = strrchr(path_str, '/') + 1;
    const size_t len = (size_t)(sep - path_str);
    char* buf = gpr_malloc(len + 2);  // '*' and NUL
    memcpy(buf, path_str, len);
    buf[len] = '*';
    buf[len + 1] = '\0';
    grpc_mdstr* wildcard_path = grpc_mdstr_from_string(buf);
    gpr_free(buf);
    value = grpc_mdstr_hash_table_get(table, wildcard_path);
    GRPC_MDSTR_UNREF(wildcard_path);
  }
  return value;
}
