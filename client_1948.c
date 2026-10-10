

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/select.h>

#define PORT 7948
#define SERVER_IP "127.0.0.1"
#define BUF_SIZE 8192
#define STORAGE_BASE "storage/IT19131948"

static ssize_t recv_line(int sock, char *buf, size_t maxlen) {
    size_t i = 0;
    while (i < maxlen - 1) {
        ssize_t r = recv(sock, buf + i, 1, 0);
        if (r <= 0) return r;
        if (buf[i] == '\n') { buf[i] = '\0'; return (ssize_t)i; }
        i++;
    }
    buf[maxlen-1] = '\0';
    return (ssize_t)i;
}
static int send_all(int sock, const void *p, size_t len) {
    size_t sent = 0;
    const char *b = p;
    while (sent < len) {
        ssize_t s = send(sock, b + sent, len - sent, 0);
        if (s <= 0) return -1;
        sent += s;
    }
    return 0;
}
static void ensure_dir(const char *path) {
    char tmp[512]; snprintf(tmp,sizeof(tmp),"%s",path);
    for (char *p = tmp + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(tmp,0755); *p = '/'; }
    mkdir(tmp,0755);
}

int main(void) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); exit(1); }
    struct sockaddr_in sa; memset(&sa,0,sizeof(sa)); sa.sin_family = AF_INET; sa.sin_port = htons(PORT);
    inet_pton(AF_INET, SERVER_IP, &sa.sin_addr);
    if (connect(sock, (struct sockaddr*)&sa, sizeof(sa)) < 0) { perror("connect"); exit(1); }

    char line[8192], rbuf[8192];
    /* read initial response */
    if (recv_line(sock, rbuf, sizeof(rbuf)) > 0) printf("%s\n", rbuf);

    ensure_dir(STORAGE_BASE);

    fd_set rfds;
    while (1) {
        FD_ZERO(&rfds); FD_SET(STDIN_FILENO, &rfds); FD_SET(sock, &rfds);
        int maxfd = sock > STDIN_FILENO ? sock : STDIN_FILENO;
        int s = select(maxfd+1, &rfds, NULL, NULL, NULL);
        if (s < 0) break;

        if (FD_ISSET(STDIN_FILENO, &rfds)) {
            if (!fgets(line, sizeof(line), stdin)) break;
            size_t L = strlen(line); if (L && line[L-1]=='\n') line[L-1]=0;
            if (strncmp(line,"SENDFILE ",9)==0) {
                char target[256], filename[256]; long filesize=0;
                int parts = sscanf(line+9,"%255s %255s %ld", target, filename, &filesize);
                if (parts < 2) { printf("Usage: SENDFILE <target> <filename> [filesize]\n"); continue; }
                FILE *f = fopen(filename,"rb");
                if (!f) { perror("fopen"); continue; }
                struct stat st; if (stat(filename,&st)==0) filesize = st.st_size;
                char header[1024]; snprintf(header,sizeof(header),"SENDFILE %s %s %ld\n", target, filename, filesize);
                if (send_all(sock, header, strlen(header)) < 0) { perror("send"); fclose(f); break; }
                char tmp[4096]; size_t r;
                while ((r = fread(tmp,1,sizeof(tmp),f)) > 0) {
                    if (send_all(sock, tmp, r) < 0) { perror("send"); break; }
                }
                fclose(f);
                /* wait for server response line */
                if (recv_line(sock, rbuf, sizeof(rbuf)) > 0) printf("%s\n", rbuf); else { printf("Server closed\n"); break; }
                continue;
            }

            /* normal command */
            char out[8192]; snprintf(out,sizeof(out), "%s\n", line);
            if (send_all(sock, out, strlen(out)) < 0) { perror("send"); break; }

            /* read immediate response for these commands */
            if (strncmp(line,"REGISTER",8)==0 || strncmp(line,"LIST",4)==0 || strncmp(line,"BCAST",5)==0 ||
                strncmp(line,"PMSG",4)==0 || strncmp(line,"JOIN",4)==0 || strncmp(line,"LEAVE",5)==0 ||
                strncmp(line,"ROOMS",5)==0 || strncmp(line,"RMSG",4)==0 || strncmp(line,"QUIT",4)==0) {
                if (recv_line(sock, rbuf, sizeof(rbuf)) > 0) {
                    printf("%s\n", rbuf);
                    if (strncmp(rbuf,"OK BYE",6)==0) { close(sock); break; }
                } else { printf("Server closed\n"); break; }
            }
        }

        if (FD_ISSET(sock, &rfds)) {
            /* read a line (header or MSG) */
            ssize_t n = recv_line(sock, rbuf, sizeof(rbuf));
            if (n <= 0) { printf("Server disconnected\n"); break; }
            printf("%s\n", rbuf);

            /* if it's a file header, parse and read exact bytes */
            if (strncmp(rbuf,"MSG FILE ",9)==0) {
                char sender[64], filename[256]; long filesize;
                if (sscanf(rbuf+9,"%63s %255s %ld", sender, filename, &filesize) == 3) {
                    char dir[512]; snprintf(dir,sizeof(dir), STORAGE_BASE "/%s", sender); ensure_dir(dir);
                    char path[1024]; snprintf(path,sizeof(path), "%s/%s", dir, filename);
                    FILE *f = fopen(path,"wb");
                    if (!f) {
                        /* consume bytes anyway */
                        long rem = filesize; char tmp[4096];
                        while (rem > 0) {
                            ssize_t r = recv(sock, tmp, rem > (long)sizeof(tmp) ? sizeof(tmp) : (size_t)rem, 0);
                            if (r <= 0) break; rem -= r;
                        }
                        printf("Failed to save incoming file\n");
                    } else {
                        long rem = filesize; char tmp[4096];
                        while (rem > 0) {
                            ssize_t r = recv(sock, tmp, rem > (long)sizeof(tmp) ? sizeof(tmp) : (size_t)rem, 0);
                            if (r <= 0) break;
                            fwrite(tmp,1,r,f);
                            rem -= r;
                        }
                        fclose(f);
                        printf("Received file %s (%ld bytes) from %s, saved to %s\n", filename, filesize, sender, path);
                    }
                }
            }
        }
    }

    close(sock);
    return 0;
}
