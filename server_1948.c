

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/stat.h>

#define PORT 7948
#define MAX_CLIENTS 64
#define BUF_SIZE 8192
#define STORAGE_BASE "storage/IT19131948"
#define LOG_FILE STORAGE_BASE "/netmsg_IT19131948.log"
#define NID_TAG "NID:1319"

typedef struct {
    int fd;
    char username[64];
    int registered;
    char buf[BUF_SIZE];   /* receive buffer */
    size_t buflen;        /* bytes currently in buf */
} Client;

typedef struct Room {
    char name[64];
    int members[MAX_CLIENTS];
    int member_count;
    struct Room *next;
} Room;

static Client clients[MAX_CLIENTS];
static Room *rooms = NULL;

/* helpers */
static void ensure_dir(const char *path) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = 0; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);
}
static void log_event(const char *fmt, ...) {
    ensure_dir(STORAGE_BASE);
    FILE *f = fopen(LOG_FILE, "a");
    if (!f) return;
    time_t t = time(NULL);
    struct tm *tm = localtime(&t);
    char ts[64]; strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", tm);
    fprintf(f, "[%s] ", ts);
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fprintf(f, "\n"); fclose(f);
    /* also console */
    va_start(ap, fmt); printf("[%s] ", ts); vprintf(fmt, ap); va_end(ap); printf("\n");
}
static ssize_t send_line_raw(int fd, const char *s) {
    if (fd < 0) return -1;
    size_t n = strlen(s);
    if (send(fd, s, n, 0) != (ssize_t)n) return -1;
    if (send(fd, "\n", 1, 0) != 1) return -1;
    return (ssize_t)(n + 1);
}
static ssize_t send_response(int fd, const char *base) {
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s %s", base, NID_TAG);
    return send_line_raw(fd, tmp);
}
static int find_client_by_name(const char *name) {
    for (int i=0;i<MAX_CLIENTS;i++) if (clients[i].fd != -1 && clients[i].registered && strcmp(clients[i].username, name)==0) return i;
    return -1;
}
static Room *find_room(const char *name) {
    for (Room *r = rooms; r; r = r->next) if (strcmp(r->name, name)==0) return r;
    return NULL;
}
static Room *create_room(const char *name) {
    Room *r = malloc(sizeof(Room));
    if (!r) return NULL;
    strncpy(r->name, name, sizeof(r->name)-1); r->name[sizeof(r->name)-1]=0;
    r->member_count = 0; for (int i=0;i<MAX_CLIENTS;i++) r->members[i]=-1;
    r->next = rooms; rooms = r; return r;
}
static void add_member(Room *r, int idx) {
    for (int i=0;i<r->member_count;i++) if (r->members[i]==idx) return;
    r->members[r->member_count++] = idx;
}
static void remove_member(Room *r, int idx) {
    int pos=-1;
    for (int i=0;i<r->member_count;i++) if (r->members[i]==idx) { pos=i; break; }
    if (pos==-1) return;
    for (int i=pos;i<r->member_count-1;i++) r->members[i]=r->members[i+1];
    r->members[--r->member_count] = -1;
}
static void notify_all_raw(const char *line, int except_idx) {
    for (int i=0;i<MAX_CLIENTS;i++) if (clients[i].fd!=-1 && clients[i].registered && i!=except_idx) send_line_raw(clients[i].fd, line);
}
static void remove_client(int idx) {
    if (idx<0||idx>=MAX_CLIENTS) return;
    if (clients[idx].fd==-1) return;
    if (clients[idx].registered) {
        char note[128]; snprintf(note,sizeof(note),"MSG LEFT %s", clients[idx].username);
        notify_all_raw(note, idx);
        log_event("DISCONNECT %s fd=%d", clients[idx].username, clients[idx].fd);
    } else log_event("DISCONNECT unregistered fd=%d", clients[idx].fd);
    /* remove from rooms */
    for (Room *r=rooms; r; r=r->next) remove_member(r, idx);
    close(clients[idx].fd);
    clients[idx].fd = -1; clients[idx].registered = 0; clients[idx].buflen = 0; clients[idx].username[0]=0;
}

/* process a single complete line (no trailing newline) */
static void process_line(int idx, const char *line) {
    int fd = clients[idx].fd;
    char cmd[16];
    if (sscanf(line, "%15s", cmd) != 1) return;

    if (strcmp(cmd,"REGISTER")==0) {
        char uname[64];
        if (sscanf(line+8," %63s", uname) != 1) { send_response(fd,"ERR 000 MALFORMED"); return; }
        if (clients[idx].registered) { char r[128]; snprintf(r,sizeof(r),"OK REGISTERED %s", clients[idx].username); send_response(fd,r); return; }
        if (find_client_by_name(uname) != -1) { send_response(fd,"ERR 001 USERNAME_TAKEN"); return; }
        strncpy(clients[idx].username, uname, sizeof(clients[idx].username)-1); clients[idx].registered = 1;
        char ok[128]; snprintf(ok,sizeof(ok),"OK REGISTERED %s", uname); send_response(fd, ok);
        char note[128]; snprintf(note,sizeof(note),"MSG JOINED %s", uname); notify_all_raw(note, idx);
        log_event("REGISTER %s fd=%d", uname, fd);
        return;
    }

    if (!clients[idx].registered) { send_response(fd,"ERR 005 NOT_REGISTERED"); return; }

    if (strcmp(cmd,"LIST")==0) {
        char out[BUF_SIZE]; strcpy(out,"OK USERS ");
        int first=1;
        for (int i=0;i<MAX_CLIENTS;i++) if (clients[i].fd!=-1 && clients[i].registered) {
            if (!first) strcat(out,","); strcat(out, clients[i].username); first=0;
        }
        send_response(fd,out); log_event("LIST by %s", clients[idx].username); return;
    }

    if (strcmp(cmd,"BCAST")==0) {
        const char *msg = line + 6;
        char out[BUF_SIZE]; snprintf(out,sizeof(out),"MSG BCAST %s %s", clients[idx].username, msg);
        notify_all_raw(out, idx);
        send_response(fd,"OK SENT"); log_event("BCAST from %s: %s", clients[idx].username, msg); return;
    }

    if (strcmp(cmd,"PMSG")==0) {
        char target[64], msg[2048];
        if (sscanf(line+5,"%63s %[^\n]", target, msg) < 2) { send_response(fd,"ERR 000 MALFORMED"); return; }
        int tidx = find_client_by_name(target);
        if (tidx == -1) { send_response(fd,"ERR 002 USER_NOT_FOUND"); return; }
        char out[BUF_SIZE]; snprintf(out,sizeof(out),"MSG PRIV %s %s", clients[idx].username, msg);
        send_line_raw(clients[tidx].fd, out);
        send_response(fd,"OK SENT"); log_event("PMSG %s -> %s: %s", clients[idx].username, target, msg); return;
    }

    if (strcmp(cmd,"JOIN")==0) {
        char room[64];
        if (sscanf(line+5,"%63s", room) != 1) { send_response(fd,"ERR 000 MALFORMED"); return; }
        Room *r = find_room(room); if (!r) r = create_room(room);
        add_member(r, idx);
        char out[128]; snprintf(out,sizeof(out),"OK JOINED %s", room); send_response(fd,out);
        log_event("JOIN %s -> %s", clients[idx].username, room); return;
    }

    if (strcmp(cmd,"LEAVE")==0) {
        char room[64];
        if (sscanf(line+6,"%63s", room) != 1) { send_response(fd,"ERR 000 MALFORMED"); return; }
        Room *r = find_room(room); if (!r) { send_response(fd,"ERR 003 ROOM_NOT_FOUND"); return; }
        remove_member(r, idx); char out[128]; snprintf(out,sizeof(out),"OK LEFT %s", room); send_response(fd,out);
        log_event("LEAVE %s -> %s", clients[idx].username, room); return;
    }

    if (strcmp(cmd,"ROOMS")==0) {
        char out[BUF_SIZE]; strcpy(out,"OK ROOMS ");
        int first=1; for (Room *r=rooms;r;r=r->next) { if (!first) strcat(out,","); strcat(out,r->name); first=0; }
        send_response(fd,out); log_event("ROOMS by %s", clients[idx].username); return;
    }

    if (strcmp(cmd,"RMSG")==0) {
        char room[64], msg[2048];
        if (sscanf(line+5,"%63s %[^\n]", room, msg) < 2) { send_response(fd,"ERR 000 MALFORMED"); return; }
        Room *r = find_room(room); if (!r) { send_response(fd,"ERR 003 ROOM_NOT_FOUND"); return; }
        char out[BUF_SIZE]; snprintf(out,sizeof(out),"MSG ROOM %s %s %s", room, clients[idx].username, msg);
        for (int i=0;i<r->member_count;i++) {
            int m = r->members[i];
            if (m>=0 && clients[m].fd!=-1 && clients[m].registered) send_line_raw(clients[m].fd, out);
        }
        send_response(fd,"OK SENT"); log_event("RMSG %s -> %s: %s", clients[idx].username, room, msg); return;
    }

    if (strcmp(cmd,"SENDFILE")==0) {
        /* header parsed; actual file bytes may already be in client's buffer after newline */
        char target[64], filename[256];
        long filesize;
        if (sscanf(line+9,"%63s %255s %ld", target, filename, &filesize) < 3) { send_response(fd,"ERR 000 MALFORMED"); return; }
        if (filesize < 0) { send_response(fd,"ERR 004 FILE_TOO_LARGE"); return; }
        int tidx = find_client_by_name(target);
        Room *r = NULL; if (tidx==-1) r = find_room(target);
        if (tidx==-1 && r==NULL) { send_response(fd,"ERR 002/003 TARGET_NOT_FOUND"); return; }

        /* prepare storage path: STORAGE_BASE/<sender>/<filename> */
        char dirpath[512]; snprintf(dirpath,sizeof(dirpath), STORAGE_BASE "/%s", clients[idx].username);
        ensure_dir(dirpath);
        char filepath[1024]; snprintf(filepath,sizeof(filepath), "%s/%s", dirpath, filename);
        FILE *outf = fopen(filepath,"wb");
        if (!outf) { send_response(fd,"ERR 004 FILE_TOO_LARGE"); return; }

        /* consume any bytes already in client's buffer (after the processed line) */
        size_t available = clients[idx].buflen;
        if (available > 0) {
            /* buffer contains data starting at buf[0]; we've already removed the processed line earlier,
               so buf currently starts with data after newline. */
            size_t towrite = (available > (size_t)filesize) ? (size_t)filesize : available;
            if (towrite > 0) {
                fwrite(clients[idx].buf, 1, towrite, outf);
                /* shift remaining bytes in buffer */
                memmove(clients[idx].buf, clients[idx].buf + towrite, available - towrite);
                clients[idx].buflen = available - towrite;
                filesize -= (long)towrite;
            }
        }

        /* read remaining bytes exactly from socket */
        char tmp[4096];
        while (filesize > 0) {
            ssize_t rcv = recv(clients[idx].fd, tmp, (filesize > (long)sizeof(tmp)) ? sizeof(tmp) : (size_t)filesize, 0);
            if (rcv <= 0) { fclose(outf); remove(filepath); send_response(fd,"ERR 006 TRANSFER_ABORTED"); log_event("SENDFILE aborted from %s", clients[idx].username); return; }
            fwrite(tmp, 1, rcv, outf);
            filesize -= rcv;
        }
        fclose(outf);

        /* forward file to target(s): send header then stream file bytes */
        FILE *inf = fopen(filepath,"rb");
        if (!inf) { send_response(fd,"ERR 004 FILE_TOO_LARGE"); return; }
        /* header: MSG FILE <sender> <filename> <size> */
        fseek(inf, 0, SEEK_END); long fsize = ftell(inf); rewind(inf);
        char header[512]; snprintf(header,sizeof(header),"MSG FILE %s %s %ld", clients[idx].username, filename, fsize);

        if (tidx != -1) {
            send_line_raw(clients[tidx].fd, header);
            while (1) {
                size_t r = fread(tmp,1,sizeof(tmp),inf);
                if (r==0) break;
                send(clients[tidx].fd, tmp, r, 0);
            }
        } else {
            for (int i=0;i<r->member_count;i++) {
                int m = r->members[i];
                if (m>=0 && clients[m].fd!=-1 && clients[m].registered) {
                    send_line_raw(clients[m].fd, header);
                    rewind(inf);
                    while (1) {
                        size_t r = fread(tmp,1,sizeof(tmp),inf);
                        if (r==0) break;
                        send(clients[m].fd, tmp, r, 0);
                    }
                }
            }
        }
        fclose(inf);

        char ok[256]; snprintf(ok,sizeof(ok),"OK FILE_RECEIVED %s", filename); send_response(fd, ok);
        log_event("SENDFILE from %s to %s filename=%s", clients[idx].username, target, filename);
        return;
    }

    if (strcmp(cmd,"QUIT")==0) {
        send_response(fd,"OK BYE");
        log_event("QUIT by %s", clients[idx].username);
        remove_client(idx);
        return;
    }

    send_response(fd,"ERR 000 UNKNOWN_COMMAND");
}

/* append received bytes to client's buffer and process complete lines */
static int append_and_process(int idx, const char *data, ssize_t len) {
    if (len <= 0) return -1;
    if (clients[idx].buflen + (size_t)len > sizeof(clients[idx].buf)) return -1; /* overflow */
    memcpy(clients[idx].buf + clients[idx].buflen, data, len);
    clients[idx].buflen += (size_t)len;

    /* process all complete lines */
    while (1) {
        char *nl = memchr(clients[idx].buf, '\n', clients[idx].buflen);
        if (!nl) break;
        size_t linelen = (size_t)(nl - clients[idx].buf);
        char line[8192];
        if (linelen >= sizeof(line)) linelen = sizeof(line)-1;
        memcpy(line, clients[idx].buf, linelen);
        line[linelen] = '\0';
        /* shift buffer left past the newline */
        size_t remain = clients[idx].buflen - (linelen + 1);
        if (remain > 0) memmove(clients[idx].buf, clients[idx].buf + linelen + 1, remain);
        clients[idx].buflen = remain;
        process_line(idx, line);
        /* note: process_line may consume additional bytes (SENDFILE reads directly from socket),
           but any bytes already in buffer after the header are preserved and used by SENDFILE logic. */
    }
    return 0;
}

int main(void) {
    for (int i=0;i<MAX_CLIENTS;i++) { clients[i].fd = -1; clients[i].registered = 0; clients[i].buflen = 0; clients[i].username[0]=0; }

    ensure_dir(STORAGE_BASE);

    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) { perror("socket"); exit(1); }
    int opt = 1; setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in sa; memset(&sa,0,sizeof(sa)); sa.sin_family = AF_INET; sa.sin_addr.s_addr = INADDR_ANY; sa.sin_port = htons(PORT);
    if (bind(listenfd, (struct sockaddr*)&sa, sizeof(sa)) < 0) { perror("bind"); exit(1); }
    if (listen(listenfd, 16) < 0) { perror("listen"); exit(1); }

    log_event("Server started on port %d", PORT);
    printf("Server listening on port %d...\n", PORT);

    fd_set rfds;
    while (1) {
        FD_ZERO(&rfds);
        FD_SET(listenfd, &rfds);
        int maxfd = listenfd;
        for (int i=0;i<MAX_CLIENTS;i++) if (clients[i].fd != -1) { FD_SET(clients[i].fd, &rfds); if (clients[i].fd > maxfd) maxfd = clients[i].fd; }

        int ready = select(maxfd+1, &rfds, NULL, NULL, NULL);
        if (ready < 0) { if (errno==EINTR) continue; perror("select"); break; }

        if (FD_ISSET(listenfd, &rfds)) {
            struct sockaddr_in cli; socklen_t clilen = sizeof(cli);
            int conn = accept(listenfd, (struct sockaddr*)&cli, &clilen);
            if (conn >= 0) {
                int slot = -1;
                for (int i=0;i<MAX_CLIENTS;i++) if (clients[i].fd == -1) { slot = i; break; }
                if (slot == -1) { send_response(conn,"ERR 007 SERVER_FULL"); close(conn); }
                else {
                    clients[slot].fd = conn; clients[slot].registered = 0; clients[slot].buflen = 0; clients[slot].username[0]=0;
                    log_event("CONNECT fd=%d", conn);
                    send_response(conn,"OK WELCOME");
                }
            }
        }

        for (int i=0;i<MAX_CLIENTS;i++) {
            if (clients[i].fd != -1 && FD_ISSET(clients[i].fd, &rfds)) {
                char tmp[4096];
                ssize_t r = recv(clients[i].fd, tmp, sizeof(tmp), 0);
                if (r <= 0) { remove_client(i); continue; }
                if (append_and_process(i, tmp, r) < 0) { remove_client(i); continue; }
            }
        }
    }

    close(listenfd);
    return 0;
}
