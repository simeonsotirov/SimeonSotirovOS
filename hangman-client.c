#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

typedef struct {
    int  fd;
    char buf[1024];
    int  head;
    int  tail;
} rbuf_t;

static void rbuf_init(rbuf_t *r, int fd)
{
    r->fd = fd;
    r->head = r->tail = 0;
}

static int rbuf_fill(rbuf_t *r)
{
    int n = (int)read(r->fd, r->buf + r->tail, sizeof(r->buf) - (size_t)r->tail);
    if (n <= 0) return -1;
    r->tail += n;
    return 0;
}

static int rbuf_getline(rbuf_t *r, char *out, size_t cap)
{
    size_t len = 0;
    for (;;) {
        for (int i = r->head; i < r->tail; i++) {
            char c = r->buf[i];
            if (c == '\n') { r->head = i + 1; out[len] = '\0'; return 0; }
            if (c != '\r' && len + 1 < cap) out[len++] = c;
        }
        if (r->head > 0) {
            memmove(r->buf, r->buf + r->head, (size_t)(r->tail - r->head));
            r->tail -= r->head;
            r->head  = 0;
        }
        if (rbuf_fill(r) < 0) return -1;
    }
}

static void sock_send(int fd, const char *s)
{
    write(fd, s, strlen(s));
}

int main(int argc, char *argv[])
{
    if (argc != 4) {
        fprintf(stderr, "Usage: %s <host> <port> <opponent-word>\n", argv[0]);
        return 1;
    }

    int port = atoi(argv[2]);

    struct sockaddr_in srv = {
        .sin_family = AF_INET,
        .sin_port   = htons((uint16_t)port),
    };
    if (inet_pton(AF_INET, argv[1], &srv.sin_addr) <= 0) {
        fprintf(stderr, "Invalid address: %s\n", argv[1]);
        return 1;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) { perror("socket"); return 1; }

    if (connect(sock, (struct sockaddr *)&srv, sizeof(srv)) < 0) {
        perror("connect");
        return 1;
    }

    char msg[256];
    snprintf(msg, sizeof(msg), "%s\n", argv[3]);
    sock_send(sock, msg);

    rbuf_t rd;
    rbuf_init(&rd, sock);

    /* Handshake: expect ACCEPT or REJECT */
    char resp[64];
    if (rbuf_getline(&rd, resp, sizeof(resp)) < 0 || strcmp(resp, "ACCEPT") != 0) {
        fprintf(stderr, "Server rejected word: %s\n", resp);
        close(sock);
        return 1;
    }

    /* Game loop: each turn is one pipe-delimited line: <masked>|<incorrect> */
    char state[512];
    for (;;) {
        if (rbuf_getline(&rd, state, sizeof(state)) < 0) break;

        char *sep = strchr(state, '|');
        if (!sep) break;
        *sep = '\0';
        char *display  = state;
        char *wrong    = sep + 1;

        printf("Word: %s\n", display);
        printf("Incorrect guesses: %s\n", wrong);
        fflush(stdout);

        if (strchr(display, '_') == NULL) break;

        char letter;
        if (scanf(" %c", &letter) != 1) break;

        snprintf(msg, sizeof(msg), "%c\n", letter);
        sock_send(sock, msg);
    }

    /* End of game: one pipe-delimited line: <WIN|LOSE|TIE>|<my_inc>|<opp_inc> */
    char result_line[512];
    if (rbuf_getline(&rd, result_line, sizeof(result_line)) < 0) goto bye;

    char *p1 = strchr(result_line, '|');
    if (!p1) goto bye;
    *p1 = '\0';

    char *p2 = strchr(p1 + 1, '|');
    if (!p2) goto bye;
    *p2 = '\0';

    char *outcome = result_line;
    char *mine    = p1 + 1;
    char *theirs  = p2 + 1;

    const char *verdict;
    if      (strcmp(outcome, "WIN")  == 0) verdict = "YOU WIN! :)";
    else if (strcmp(outcome, "LOSE") == 0) verdict = "You Lose! :(";
    else                                    verdict = "Tie :/";

    printf("%s\n", verdict);
    printf("Your incorrect guesses: %s\n", mine);
    printf("Opponent's incorrect guesses: %s\n", theirs);
    fflush(stdout);

bye:
    close(sock);
    return 0;
}
