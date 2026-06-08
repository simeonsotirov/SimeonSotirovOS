#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <semaphore.h>
#include "game.h"

typedef struct {
    secret_word_t   words[2];
    int             fds[2];
    sem_t           barrier;
    int             arrived;
    pthread_mutex_t arrive_lock;
} session_t;

typedef struct {
    session_t *sess;
    int        player;
} worker_t;

static ssize_t read_line(int fd, char *buf, size_t max)
{
    size_t n = 0;
    char c;
    while (n < max - 1) {
        if (read(fd, &c, 1) <= 0) return -1;
        if (c == '\n') break;
        if (c != '\r') buf[n++] = c;
    }
    buf[n] = '\0';
    return (ssize_t)n;
}

static void build_csv(letter_set_t set, char *out, size_t cap)
{
    size_t pos = 0;
    int    any = 0;
    for (int ch = 'a'; ch <= 'z'; ch++) {
        if (!letter_set_contains(set, (char)ch)) continue;
        if (any && pos + 2 < cap) { out[pos++] = ','; out[pos++] = ' '; }
        if (pos + 1 < cap) out[pos++] = (char)ch;
        any = 1;
    }
    out[pos] = '\0';
}

static void push_state(int fd, const secret_word_t *w)
{
    char masked[256];
    size_t wlen = w->word_length < sizeof(masked) - 1 ? w->word_length : sizeof(masked) - 1;
    for (size_t k = 0; k < wlen; k++) {
        char revealed;
        masked[k] = (secret_word_letter_at(w, k, &revealed) == SECRET_WORD_LETTER_REVEALED)
                    ? revealed : '_';
    }
    masked[wlen] = '\0';

    char inc[128];
    build_csv(w->incorrect_guesses, inc, sizeof(inc));

    char pkt[512];
    int  plen = snprintf(pkt, sizeof(pkt), "%s|%s\n", masked, inc);
    write(fd, pkt, (size_t)plen);
}

static void *player_thread(void *vp)
{
    worker_t  *wa   = (worker_t *)vp;
    session_t *sess = wa->sess;
    int        me   = wa->player;
    int        fd   = sess->fds[me];
    secret_word_t *sw = &sess->words[1 - me];

    push_state(fd, sw);

    char buf[16];
    while (!secret_word_is_solved(sw)) {
        if (read_line(fd, buf, sizeof(buf)) < 0) break;
        char g = normalize(buf[0]);
        if (is_letter(g))
            secret_word_guess(sw, g);
        push_state(fd, sw);
    }

    pthread_mutex_lock(&sess->arrive_lock);
    int count = ++sess->arrived;
    pthread_mutex_unlock(&sess->arrive_lock);

    if (count == 2) {
        sem_post(&sess->barrier);
        sem_post(&sess->barrier);
    }
    sem_wait(&sess->barrier);

    size_t my_err  = secret_word_incorrect_guess_count(&sess->words[1 - me]);
    size_t opp_err = secret_word_incorrect_guess_count(&sess->words[me]);

    const char *result;
    if      (my_err < opp_err) result = "WIN";
    else if (my_err > opp_err) result = "LOSE";
    else                        result = "TIE";

    char my_inc[128], opp_inc[128];
    build_csv(sess->words[1 - me].incorrect_guesses, my_inc,  sizeof(my_inc));
    build_csv(sess->words[me].incorrect_guesses,     opp_inc, sizeof(opp_inc));

    char pkt[512];
    int  plen = snprintf(pkt, sizeof(pkt), "%s|%s|%s\n", result, my_inc, opp_inc);
    write(fd, pkt, (size_t)plen);
    close(fd);
    return NULL;
}

int main(int argc, char *argv[])
{
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <port>\n", argv[0]);
        return 1;
    }

    int srv_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv_fd < 0) { perror("socket"); return 1; }

    int yes = 1;
    setsockopt(srv_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in sa = {
        .sin_family      = AF_INET,
        .sin_addr.s_addr = INADDR_ANY,
        .sin_port        = htons((uint16_t)atoi(argv[1])),
    };
    if (bind(srv_fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) { perror("bind"); return 1; }
    if (listen(srv_fd, 4) < 0) { perror("listen"); return 1; }

    printf("Listening on %d...\n", atoi(argv[1]));
    fflush(stdout);

    session_t sess;
    memset(&sess, 0, sizeof(sess));
    sem_init(&sess.barrier, 0, 0);
    pthread_mutex_init(&sess.arrive_lock, NULL);

    int  joined = 0;
    char wbuf[256];

    while (joined < 2) {
        int cfd = accept(srv_fd, NULL, NULL);
        if (cfd < 0) continue;

        if (read_line(cfd, wbuf, sizeof(wbuf)) < 0) { close(cfd); continue; }

        secret_word_t tmp;
        if (!secret_word_init_from_c_string(&tmp, wbuf)) {
            write(cfd, "REJECT\n", 7);
            close(cfd);
            continue;
        }

        write(cfd, "ACCEPT\n", 7);
        sess.words[joined] = tmp;
        sess.fds[joined]   = cfd;
        joined++;
    }

    close(srv_fd);

    worker_t   wa[2]      = {{&sess, 0}, {&sess, 1}};
    pthread_t  threads[2];
    pthread_create(&threads[0], NULL, player_thread, &wa[0]);
    pthread_create(&threads[1], NULL, player_thread, &wa[1]);
    pthread_join(threads[0], NULL);
    pthread_join(threads[1], NULL);

    secret_word_free(&sess.words[0]);
    secret_word_free(&sess.words[1]);
    sem_destroy(&sess.barrier);
    pthread_mutex_destroy(&sess.arrive_lock);
    return 0;
}
