/*
 * conga-remote.c — Web remote control server for Conga robot vacuum
 *
 * Features:
 *   - Manual control: forward, backward, rotate CW, rotate CCW
 *   - Learning mode: record movement sequences, save as named patterns
 *   - Pattern playback: replay saved movement patterns
 *   - Proxies Congatudo API to avoid CORS issues
 *   - Single HTML frontend served from /mnt/UDISK/conga-remote/www/
 *
 * Build (cross-compile for ARM):
 *   arm-linux-gnueabihf-gcc -static -O2 -o conga-remote conga-remote.c
 *   arm-linux-gnueabihf-strip conga-remote
 *
 * Run:
 *   ./conga-remote <port> [-d]
 *   -d = daemon mode
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>

#define MAX_REQUEST 8192
#define MAX_RESPONSE 65536
#define MAX_BODY 4096
#define MAX_PATTERNS 64
#define MAX_PATTERN_NAME 64
#define MAX_PATTERN_STEPS 512
#define WWW_DIR "/mnt/UDISK/conga-remote/www"
#define PATTERNS_DIR "/mnt/UDISK/conga-remote/patterns"
#define CONGA_HOST "127.0.0.1"
#define CONGA_PORT 80

#define CMD_FORWARD 1
#define CMD_BACKWARD 2
#define CMD_ROTATE_CW 3
#define CMD_ROTATE_CCW 4

typedef struct {
    int command;
    int duration_ms;
} pattern_step_t;

typedef struct {
    char name[MAX_PATTERN_NAME];
    pattern_step_t steps[MAX_PATTERN_STEPS];
    int num_steps;
} pattern_t;

static int g_learning = 0;
static pattern_t g_recording;
static pattern_t g_patterns[MAX_PATTERNS];
static int g_num_patterns = 0;
static int g_playing = 0;
static volatile sig_atomic_t g_running = 1;
static int g_server_fd = -1;

static int proxy_to_congatudo(const char *method, const char *path,
                               const char *body, int body_len,
                               char *response, int *resp_len);
static void load_patterns(void);
static void save_pattern(pattern_t *pat);
static void delete_pattern(const char *name);
static void handle_request(int client_fd, const char *request, int req_len);
static int read_file_to_buf(const char *path, char *buf, int max_len, int *out_len);
static const char *get_content_type(const char *path);
static void send_response(int fd, int status, const char *status_text,
                          const char *content_type, const char *body, int body_len);
static void send_error(int fd, int status, const char *text);

static void ensure_dir(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) mkdir(path, 0755);
}

static void load_patterns(void) {
    DIR *d;
    struct dirent *ent;
    char path[512];

    ensure_dir(PATTERNS_DIR);
    g_num_patterns = 0;

    d = opendir(PATTERNS_DIR);
    if (!d) return;

    while ((ent = readdir(d)) != NULL && g_num_patterns < MAX_PATTERNS) {
        if (ent->d_name[0] == '.') continue;
        char *dot = strrchr(ent->d_name, '.');
        if (!dot || strcmp(dot, ".json") != 0) continue;

        snprintf(path, sizeof(path), "%s/%s", PATTERNS_DIR, ent->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;

        char buf[16384];
        int n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = '\0';
        fclose(f);

        pattern_t *pat = &g_patterns[g_num_patterns];
        char *p = strstr(buf, "\"name\":\"");
        if (!p) continue;
        p += 8;
        char *e = strchr(p, '"');
        if (!e) continue;
        int nlen = e - p;
        if (nlen >= MAX_PATTERN_NAME) nlen = MAX_PATTERN_NAME - 1;
        memcpy(pat->name, p, nlen);
        pat->name[nlen] = '\0';

        pat->num_steps = 0;
        char *steps_start = strstr(buf, "\"steps\":[");
        if (!steps_start) { pat->name[0] = '\0'; continue; }
        steps_start += 9;

        char *sp = steps_start;
        while (*sp && *sp != ']' && pat->num_steps < MAX_PATTERN_STEPS) {
            char *obj = strchr(sp, '{');
            if (!obj || obj > strchr(sp, ']')) break;
            int cmd = 0, ms = 0;
            char *c = strstr(obj, "\"cmd\":");
            char *m = strstr(obj, "\"ms\":");
            if (c && c < strchr(obj, '}')) cmd = atoi(c + 6);
            if (m && m < strchr(obj, '}')) ms = atoi(m + 5);
            pat->steps[pat->num_steps].command = cmd;
            pat->steps[pat->num_steps].duration_ms = ms;
            pat->num_steps++;
            sp = strchr(obj, '}');
            if (sp) sp++;
        }
        g_num_patterns++;
    }
    closedir(d);
}

static void save_pattern(pattern_t *pat) {
    ensure_dir(PATTERNS_DIR);
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.json", PATTERNS_DIR, pat->name);
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "{\"name\":\"%s\",\"steps\":[", pat->name);
    for (int i = 0; i < pat->num_steps; i++) {
        if (i > 0) fprintf(f, ",");
        fprintf(f, "{\"cmd\":%d,\"ms\":%d}", pat->steps[i].command, pat->steps[i].duration_ms);
    }
    fprintf(f, "]}");
    fclose(f);
}

static void delete_pattern(const char *name) {
    char path[768];
    snprintf(path, sizeof(path), "%s/%s.json", PATTERNS_DIR, name);
    unlink(path);
}

static int proxy_to_congatudo(const char *method, const char *path,
                               const char *body, int body_len,
                               char *response, int *resp_len) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    struct timeval tv = {5, 0};
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(CONGA_PORT);
    inet_pton(AF_INET, CONGA_HOST, &addr.sin_addr);

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock); return -1;
    }

    char req_buf[MAX_REQUEST];
    int req_len;
    if (body_len > 0) {
        req_len = snprintf(req_buf, sizeof(req_buf),
            "%s %s HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\n"
            "Content-Length: %d\r\nConnection: close\r\n\r\n",
            method, path, CONGA_HOST, body_len);
        int remaining = sizeof(req_buf) - req_len - 1;
        if (body_len < remaining) {
            memcpy(req_buf + req_len, body, body_len);
            req_len += body_len;
        }
    } else {
        req_len = snprintf(req_buf, sizeof(req_buf),
            "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
            method, path, CONGA_HOST);
    }
    send(sock, req_buf, req_len, 0);

    *resp_len = 0;
    int n;
    while ((n = recv(sock, response + *resp_len, MAX_RESPONSE - *resp_len - 1, 0)) > 0)
        *resp_len += n;
    response[*resp_len] = '\0';
    close(sock);
    return 0;
}

static char *http_response_body(char *resp, int len) {
    char *p = strstr(resp, "\r\n\r\n");
    if (p) return p + 4;
    p = strstr(resp, "\n\n");
    if (p) return p + 2;
    return resp;
}

static int send_manual_action(const char *action_json) {
    char resp[MAX_RESPONSE]; int rlen;
    return proxy_to_congatudo("PUT", "/api/v2/robot/capabilities/ManualControlCapability",
        action_json, strlen(action_json), resp, &rlen);
}

static const char *cmd_to_movement(int cmd) {
    switch (cmd) {
        case CMD_FORWARD: return "forward";
        case CMD_BACKWARD: return "backward";
        case CMD_ROTATE_CW: return "rotate_clockwise";
        case CMD_ROTATE_CCW: return "rotate_counterclockwise";
        default: return "forward";
    }
}

static const char *get_content_type(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (strcmp(ext, ".html") == 0 || strcmp(ext, ".htm") == 0) return "text/html";
    if (strcmp(ext, ".css") == 0) return "text/css";
    if (strcmp(ext, ".js") == 0) return "application/javascript";
    if (strcmp(ext, ".json") == 0) return "application/json";
    if (strcmp(ext, ".png") == 0) return "image/png";
    if (strcmp(ext, ".svg") == 0) return "image/svg+xml";
    if (strcmp(ext, ".ico") == 0) return "image/x-icon";
    return "application/octet-stream";
}

static void send_response(int fd, int status, const char *status_text,
                          const char *content_type, const char *body, int body_len) {
    char header[1024];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\nAccess-Control-Allow-Methods: GET,PUT,POST,DELETE,OPTIONS\r\n"
        "Access-Control-Allow-Headers: Content-Type\r\nConnection: close\r\n\r\n",
        status, status_text, content_type, body_len);
    send(fd, header, hlen, 0);
    if (body_len > 0) send(fd, body, body_len, 0);
}

static void send_error(int fd, int status, const char *text) {
    char body[256];
    int blen = snprintf(body, sizeof(body), "{\"error\":\"%s\"}", text);
    send_response(fd, status, "Error", "application/json", body, blen);
}

static int read_file_to_buf(const char *path, char *buf, int max_len, int *out_len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    *out_len = read(fd, buf, max_len);
    close(fd);
    return (*out_len > 0) ? 0 : -1;
}

static void parse_request_line(const char *req, char *method, int msize, char *path, int psize) {
    const char *sp1 = strchr(req, ' ');
    if (!sp1) { method[0] = '\0'; path[0] = '/'; path[1] = '\0'; return; }
    int mlen = sp1 - req;
    if (mlen >= msize) mlen = msize - 1;
    memcpy(method, req, mlen); method[mlen] = '\0';
    const char *sp2 = strchr(sp1 + 1, ' ');
    if (!sp2) { strcpy(path, "/"); return; }
    int plen = sp2 - sp1 - 1;
    if (plen >= psize) plen = psize - 1;
    memcpy(path, sp1 + 1, plen); path[plen] = '\0';
}

static const char *get_request_body(const char *req, int req_len, int *body_len) {
    const char *p = strstr(req, "\r\n\r\n");
    if (p) { *body_len = req_len - (p + 4 - req); return p + 4; }
    *body_len = 0; return "";
}

static void api_move(int fd, int cmd, int duration_ms) {
    char json[256];
    snprintf(json, sizeof(json), "{\"action\":\"move\",\"movementCommand\":\"%s\"}", cmd_to_movement(cmd));
    if (send_manual_action(json) < 0) { send_error(fd, 502, "Failed to reach Congatudo"); return; }
    if (g_learning && g_recording.num_steps < MAX_PATTERN_STEPS) {
        g_recording.steps[g_recording.num_steps].command = cmd;
        g_recording.steps[g_recording.num_steps].duration_ms = duration_ms;
        g_recording.num_steps++;
    }
    send_response(fd, 200, "OK", "application/json", "{\"ok\":true}", 9);
}

static void api_enable_manual(int fd) {
    if (send_manual_action("{\"action\":\"enable\"}") < 0) { send_error(fd, 502, "Failed to reach Congatudo"); return; }
    send_response(fd, 200, "OK", "application/json", "{\"ok\":true}", 9);
}

static void api_disable_manual(int fd) {
    if (send_manual_action("{\"action\":\"disable\"}") < 0) { send_error(fd, 502, "Failed to reach Congatudo"); return; }
    if (g_learning) g_learning = 0;
    send_response(fd, 200, "OK", "application/json", "{\"ok\":true}", 9);
}

static void api_start_learning(int fd) {
    g_learning = 1;
    memset(&g_recording, 0, sizeof(g_recording));
    send_response(fd, 200, "OK", "application/json", "{\"ok\":true,\"learning\":true}", 21);
}

static void api_stop_learning(int fd, const char *name, int name_len) {
    if (!g_learning) { send_error(fd, 400, "Not in learning mode"); return; }
    if (g_recording.num_steps == 0) { g_learning = 0; send_error(fd, 400, "No steps recorded"); return; }
    g_learning = 0;
    int nlen = name_len;
    if (nlen >= MAX_PATTERN_NAME) nlen = MAX_PATTERN_NAME - 1;
    memcpy(g_recording.name, name, nlen);
    g_recording.name[nlen] = '\0';
    save_pattern(&g_recording);
    if (g_num_patterns < MAX_PATTERNS) { g_patterns[g_num_patterns] = g_recording; g_num_patterns++; }
    char resp[256];
    int rlen = snprintf(resp, sizeof(resp), "{\"ok\":true,\"name\":\"%s\",\"steps\":%d}", g_recording.name, g_recording.num_steps);
    send_response(fd, 200, "OK", "application/json", resp, rlen);
}

static void api_list_patterns(int fd) {
    char resp[16384]; int pos = 0, max = sizeof(resp) - 1;
    pos += snprintf(resp + pos, max - pos, "{\"patterns\":[");
    for (int i = 0; i < g_num_patterns; i++) {
        if (i > 0) resp[pos++] = ',';
        pos += snprintf(resp + pos, max - pos, "{\"name\":\"%s\",\"steps\":%d}", g_patterns[i].name, g_patterns[i].num_steps);
        if (pos >= max) break;
    }
    pos += snprintf(resp + pos, max - pos, "],\"learning\":%s}", g_learning ? "true" : "false");
    resp[pos] = '\0';
    send_response(fd, 200, "OK", "application/json", resp, pos);
}

static void api_get_pattern(int fd, const char *name) {
    for (int i = 0; i < g_num_patterns; i++) {
        if (strcmp(g_patterns[i].name, name) == 0) {
            char resp[16384]; int pos = 0, max = sizeof(resp) - 1;
            pos += snprintf(resp + pos, max - pos, "{\"name\":\"%s\",\"steps\":[", g_patterns[i].name);
            for (int j = 0; j < g_patterns[i].num_steps; j++) {
                if (pos >= max) break;
                if (j > 0) resp[pos++] = ',';
                pos += snprintf(resp + pos, max - pos, "{\"cmd\":%d,\"ms\":%d}", g_patterns[i].steps[j].command, g_patterns[i].steps[j].duration_ms);
            }
            pos += snprintf(resp + pos, max - pos, "]}");
            resp[pos] = '\0';
            send_response(fd, 200, "OK", "application/json", resp, pos);
            return;
        }
    }
    send_error(fd, 404, "Pattern not found");
}

static void api_delete_pattern(int fd, const char *name) {
    delete_pattern(name);
    for (int i = 0; i < g_num_patterns; i++) {
        if (strcmp(g_patterns[i].name, name) == 0) {
            memmove(&g_patterns[i], &g_patterns[i+1], (g_num_patterns - i - 1) * sizeof(pattern_t));
            g_num_patterns--; break;
        }
    }
    send_response(fd, 200, "OK", "application/json", "{\"ok\":true}", 9);
}

static void api_play_pattern(int fd, const char *name) {
    pattern_t *pat = NULL;
    for (int i = 0; i < g_num_patterns; i++) {
        if (strcmp(g_patterns[i].name, name) == 0) { pat = &g_patterns[i]; break; }
    }
    if (!pat) { send_error(fd, 404, "Pattern not found"); return; }

    /* Fork child to play pattern so we don't block the server */
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        send_manual_action("{\"action\":\"enable\"}");
        for (int i = 0; i < pat->num_steps; i++) {
            char json[256];
            snprintf(json, sizeof(json), "{\"action\":\"move\",\"movementCommand\":\"%s\"}", cmd_to_movement(pat->steps[i].command));
            send_manual_action(json);
            int ms = pat->steps[i].duration_ms > 0 ? pat->steps[i].duration_ms : 300;
            struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
            nanosleep(&ts, NULL);
        }
        send_manual_action("{\"action\":\"disable\"}");
        _exit(0);
    }
    g_playing = pid;
    char resp[256];
    int rlen = snprintf(resp, sizeof(resp), "{\"ok\":true,\"pid\":%d,\"name\":\"%s\"}", pid, name);
    send_response(fd, 200, "OK", "application/json", resp, rlen);
}

static void api_stop_play(int fd) {
    if (g_playing > 0) { kill(g_playing, SIGTERM); g_playing = 0; }
    send_manual_action("{\"action\":\"disable\"}");
    send_response(fd, 200, "OK", "application/json", "{\"ok\":true}", 9);
}

static void api_status(int fd) {
    char resp[512];
    int rlen = snprintf(resp, sizeof(resp),
        "{\"learning\":%s,\"recording_steps\":%d,\"playing\":%s}",
        g_learning ? "true" : "false", g_recording.num_steps,
        (g_playing > 0 && kill(g_playing, 0) == 0) ? "true" : "false");
    send_response(fd, 200, "OK", "application/json", resp, rlen);
}

static void api_robot_state(int fd) {
    char resp[MAX_RESPONSE]; int rlen;
    if (proxy_to_congatudo("GET", "/api/v2/robot/state", NULL, 0, resp, &rlen) < 0) {
        send_error(fd, 502, "Failed to reach Congatudo"); return;
    }
    char *body = http_response_body(resp, rlen);
    int blen = rlen - (body - resp);
    send_response(fd, 200, "OK", "application/json", body, blen > 0 ? blen : 0);
}

static void handle_request(int client_fd, const char *request, int req_len) {
    char method[16], path[512];
    parse_request_line(request, method, sizeof(method), path, sizeof(path));

    if (strcmp(method, "OPTIONS") == 0) {
        send_response(client_fd, 204, "No Content", "text/plain", "", 0);
        return;
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/enable") == 0) { api_enable_manual(client_fd); return; }
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/disable") == 0) { api_disable_manual(client_fd); return; }

    if (strcmp(method, "POST") == 0) {
        int cmd = -1;
        if (strcmp(path, "/api/forward") == 0) cmd = CMD_FORWARD;
        else if (strcmp(path, "/api/backward") == 0) cmd = CMD_BACKWARD;
        else if (strcmp(path, "/api/right") == 0) cmd = CMD_ROTATE_CW;
        else if (strcmp(path, "/api/left") == 0) cmd = CMD_ROTATE_CCW;
        if (cmd >= 0) {
            int body_len; const char *body = get_request_body(request, req_len, &body_len);
            int duration = 300;
            const char *dp = strstr(body, "\"duration\"");
            if (dp) { const char *col = strchr(dp + 10, ':'); if (col) duration = atoi(col + 1); }
            api_move(client_fd, cmd, duration);
            return;
        }
    }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/learn/start") == 0) { api_start_learning(client_fd); return; }

    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/learn/stop") == 0) {
        int body_len; const char *body = get_request_body(request, req_len, &body_len);
        char name[MAX_PATTERN_NAME] = "unnamed";
        const char *np = strstr(body, "\"name\"");
        if (np) {
            const char *q1 = strchr(np + 6, '"');
            if (q1) { const char *q2 = strchr(q1 + 1, '"');
                if (q2) { int nlen = q2 - q1 - 1; if (nlen >= MAX_PATTERN_NAME) nlen = MAX_PATTERN_NAME - 1;
                    memcpy(name, q1 + 1, nlen); name[nlen] = '\0'; } }
        }
        api_stop_learning(client_fd, name, strlen(name));
        return;
    }

    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/patterns") == 0) { api_list_patterns(client_fd); return; }
    if (strcmp(method, "GET") == 0 && strncmp(path, "/api/patterns/", 14) == 0) { api_get_pattern(client_fd, path + 14); return; }
    if (strcmp(method, "DELETE") == 0 && strncmp(path, "/api/patterns/", 14) == 0) { api_delete_pattern(client_fd, path + 14); return; }
    if (strcmp(method, "POST") == 0 && strncmp(path, "/api/play/", 10) == 0) { api_play_pattern(client_fd, path + 10); return; }
    if (strcmp(method, "POST") == 0 && strcmp(path, "/api/stop") == 0) { api_stop_play(client_fd); return; }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/status") == 0) { api_status(client_fd); return; }
    if (strcmp(method, "GET") == 0 && strcmp(path, "/api/robot") == 0) { api_robot_state(client_fd); return; }

    if (strcmp(method, "GET") == 0) {
        char filepath[512]; const char *fpath = path;
        if (strcmp(path, "/") == 0) fpath = "/index.html";
        if (strstr(fpath, "..")) { send_error(client_fd, 403, "Forbidden"); return; }
        snprintf(filepath, sizeof(filepath), "%s%s", WWW_DIR, fpath);
        char *filebuf = malloc(MAX_RESPONSE);
        if (!filebuf) { send_error(client_fd, 500, "Out of memory"); return; }
        int file_len;
        if (read_file_to_buf(filepath, filebuf, MAX_RESPONSE, &file_len) == 0)
            send_response(client_fd, 200, "OK", get_content_type(filepath), filebuf, file_len);
        else
            send_error(client_fd, 404, "File not found");
        free(filebuf);
        return;
    }

    send_error(client_fd, 405, "Method not allowed");
}

static void sig_handler(int sig) { (void)sig; g_running = 0; if (g_server_fd >= 0) { close(g_server_fd); g_server_fd = -1; } }

static void daemonize(void) {
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); exit(1); }
    if (pid > 0) exit(0);
    setsid();
    close(STDIN_FILENO); close(STDOUT_FILENO); close(STDERR_FILENO);
    open("/dev/null", O_RDONLY); open("/dev/null", O_WRONLY); open("/dev/null", O_WRONLY);
}

int main(int argc, char *argv[]) {
    if (argc < 2) { fprintf(stderr, "Usage: %s <port> [-d]\n", argv[0]); return 1; }
    int port = atoi(argv[1]);
    int daemon = (argc > 2 && strcmp(argv[2], "-d") == 0);

    signal(SIGCHLD, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    load_patterns();

    g_server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_server_fd < 0) { perror("socket"); return 1; }
    int opt = 1;
    setsockopt(g_server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(g_server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) { perror("bind"); return 1; }
    if (listen(g_server_fd, 10) < 0) { perror("listen"); return 1; }
    if (daemon) daemonize();

    fprintf(stderr, "conga-remote: listening on port %d\n", port);

    while (g_running) {
        struct sockaddr_in client_addr; socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(g_server_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) { if (errno == EINTR) continue; if (!g_running) break; continue; }

        struct timeval tv = {5, 0};
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        char request[MAX_REQUEST]; int req_len = 0, n;
        while ((n = recv(client_fd, request + req_len, MAX_REQUEST - req_len - 1, 0)) > 0) {
            req_len += n; request[req_len] = '\0';
            if (strstr(request, "\r\n\r\n")) break;
        }
        request[req_len] = '\0';
        if (req_len > 0) handle_request(client_fd, request, req_len);
        close(client_fd);
    }

    if (g_server_fd >= 0) close(g_server_fd);
    return 0;
}