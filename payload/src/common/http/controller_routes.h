// SPDX-License-Identifier: GPL-3.0-or-later
/* Private route helpers, included after reply/profile_reply in server.c. */
#include "common/input/controller.h"

#define CONTROLLER_PACKET_LIMIT 128

typedef struct {
  char token[PBS_INPUT_TOKEN_SIZE];
  size_t content_length;
  const char *body;
  int has_marker;
  int has_token;
  int has_content_length;
} controller_request;

static int controller_copy_header_value(const char *begin, const char *end, char *output,
                                        size_t capacity) {
  while (begin < end && (*begin == ' ' || *begin == '\t')) {
    begin++;
  }
  while (end > begin && (end[-1] == ' ' || end[-1] == '\t')) {
    end--;
  }

  size_t length = (size_t)(end - begin);
  if (length >= capacity) {
    return -1;
  }
  memcpy(output, begin, length);
  output[length] = '\0';
  return 0;
}

static int controller_parse_content_length(const char *begin, const char *end,
                                           controller_request *parsed, const char **error) {
  char value[32];
  if (parsed->has_content_length || controller_copy_header_value(begin, end, value, sizeof value) ||
      value[0] < '0' || value[0] > '9') {
    *error = "Invalid content length";
    return 400;
  }

  errno = 0;
  char *tail = NULL;
  unsigned long length = strtoul(value, &tail, 10);
  if (errno || *tail || length > CONTROLLER_PACKET_LIMIT) {
    *error = "Controller packet too large";
    return 413;
  }
  parsed->has_content_length = 1;
  parsed->content_length = (size_t)length;
  return 200;
}

static int controller_parse_header(const char *line, const char *end, controller_request *parsed,
                                   const char **error) {
  static const char marker_header[] = "X-PBS-Input:";
  static const char token_header[] = "X-PBS-Controller:";
  static const char length_header[] = "Content-Length:";
  static const char encoding_header[] = "Transfer-Encoding:";

  if (!strncasecmp(line, marker_header, sizeof marker_header - 1)) {
    char marker[8];
    const char *value = line + sizeof marker_header - 1;
    if (parsed->has_marker || controller_copy_header_value(value, end, marker, sizeof marker) ||
        strcmp(marker, "1")) {
      *error = "Controller request header required";
      return 403;
    }
    parsed->has_marker = 1;
  } else if (!strncasecmp(line, token_header, sizeof token_header - 1)) {
    const char *value = line + sizeof token_header - 1;
    if (parsed->has_token ||
        controller_copy_header_value(value, end, parsed->token, sizeof parsed->token)) {
      *error = "Invalid controller token";
      return 400;
    }
    parsed->has_token = 1;
  } else if (!strncasecmp(line, length_header, sizeof length_header - 1)) {
    return controller_parse_content_length(line + sizeof length_header - 1, end, parsed, error);
  } else if (!strncasecmp(line, encoding_header, sizeof encoding_header - 1)) {
    *error = "Chunked controller packets are not supported";
    return 400;
  }
  return 200;
}

static int controller_parse_request(const char *request, controller_request *parsed,
                                    const char **error) {
  const char *headers_end = strstr(request, "\r\n\r\n");
  if (!headers_end) {
    *error = "Incomplete request";
    return 400;
  }
  memset(parsed, 0, sizeof *parsed);
  parsed->body = headers_end + 4;

  const char *line = strstr(request, "\r\n") + 2;
  while (line < headers_end) {
    const char *line_end = strstr(line, "\r\n");
    if (!line_end || line_end > headers_end) {
      *error = "Invalid headers";
      return 400;
    }
    int status = controller_parse_header(line, line_end, parsed, error);
    if (status != 200) {
      return status;
    }
    line = line_end + 2;
  }

  /* A custom header prevents cross-origin forms and simple browser fetches
   * from registering a controller. These routes do not grant CORS access. */
  if (!parsed->has_marker) {
    *error = "Controller request header required";
    return 403;
  }
  return 200;
}

static int controller_read_state(int fd, const char *request, size_t received,
                                 const controller_request *parsed, uint32_t *sequence,
                                 pbs_input_state *state, const char **error) {
  char body[CONTROLLER_PACKET_LIMIT + 1];
  size_t body_offset = (size_t)(parsed->body - request);
  size_t available = received > body_offset ? received - body_offset : 0;
  if (available > parsed->content_length) {
    available = parsed->content_length;
  }
  memcpy(body, parsed->body, available);

  while (available < parsed->content_length) {
    ssize_t count = recv(fd, body + available, parsed->content_length - available, 0);
    if (count <= 0) {
      *error = "Incomplete controller packet";
      return 400;
    }
    available += (size_t)count;
  }
  body[parsed->content_length] = '\0';

  if (memchr(body, '\0', parsed->content_length) || pbs_input_parse_state(body, sequence, state)) {
    *error = "Invalid controller state";
    return 400;
  }
  return 200;
}

static const char *controller_result_message(int status) {
  switch (status) {
  case 200:
    return "OK";
  case 501:
    return "Controller input unavailable on this platform";
  case 403:
    return "Controller belongs to another session";
  case 409:
    return "Controller busy, expired or sequence out of order";
  default:
    return "Controller operation failed; see controller.log";
  }
}

static void controller_reply_status(int fd) {
  pbs_input_status status;
  pbs_input_get_status(&status);

  char body[256];
  int length = snprintf(body, sizeof body,
                        "{\"supported\":%s,\"connected\":%s,\"cleanup_required\":%s,"
                        "\"last_rc\":%d,\"packets\":%llu}",
                        status.supported ? "true" : "false", status.connected ? "true" : "false",
                        status.cleanup_required ? "true" : "false", status.last_rc,
                        (unsigned long long)status.packets);
  reply(fd, "application/json", body, (size_t)length);
}

static void controller_connect(int fd) {
  char token[PBS_INPUT_TOKEN_SIZE];
  int status = pbs_input_connect(token);
  if (status != 200) {
    profile_reply(fd, status, controller_result_message(status));
    return;
  }
  char body[64];
  int length = snprintf(body, sizeof body, "{\"token\":\"%s\"}", token);
  reply(fd, "application/json", body, (size_t)length);
}

static void input_route(int fd, const char *method, const char *path, const char *request,
                        size_t received) {
  if (!strcmp(method, "GET") && !strcmp(path, "/input/status")) {
    controller_reply_status(fd);
    return;
  }
  if (strcmp(method, "POST")) {
    profile_reply(fd, 405, "Use POST for controller commands");
    return;
  }

  int connecting = !strcmp(path, "/input/connect");
  int disconnecting = !strcmp(path, "/input/disconnect");
  if (!connecting && !disconnecting && strcmp(path, "/input/state")) {
    profile_reply(fd, 404, "Unknown controller command");
    return;
  }

  controller_request parsed;
  const char *error = NULL;
  int status = controller_parse_request(request, &parsed, &error);
  if (status != 200) {
    profile_reply(fd, status, error);
    return;
  }
  if ((connecting || disconnecting) && parsed.content_length) {
    profile_reply(fd, 400, "Unexpected command body");
    return;
  }
  if (!connecting && (!parsed.has_token || strlen(parsed.token) != 32)) {
    profile_reply(fd, 403, "Controller owner token required");
    return;
  }

  if (connecting) {
    controller_connect(fd);
    return;
  }
  if (disconnecting) {
    status = pbs_input_disconnect(parsed.token);
  } else {
    uint32_t sequence;
    pbs_input_state state;
    status = controller_read_state(fd, request, received, &parsed, &sequence, &state, &error);
    if (status != 200) {
      profile_reply(fd, status, error);
      return;
    }
    status = pbs_input_submit(parsed.token, sequence, &state);
  }
  profile_reply(fd, status, controller_result_message(status));
}
