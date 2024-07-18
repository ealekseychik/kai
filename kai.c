#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>


#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#define ABUF_INIT {NULL, 0}
#define CTRL_KEY(k) ((k) & 0x1f)
#define KAI_VERSION "0.0.1"
#define KAI_TAB_STOP 4

typedef struct erow {
    int size;
    int rsize;
    char *chars;
    char *render;
} erow;

enum editorKey {
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    HOME_KEY,
    END_KEY,
    DEL_KEY,
    PAGE_UP,
    PAGE_DOWN
};

struct abuf {
    char *b;
    int len;
};

struct editorConfig {
    // Cursor position
    int cx, cy;
    // Cursor position in the file
    int rx;
    // Editor rows and cols offset
    int rowoff, coloff;
    // Terminal window size
    int screenrows, screencols;
    // Termios original terminal state
    struct termios orig_state;
    // Editor rows
    int numrows;
    erow *row;
    // Status message
    char *filename;
    char statusmsg[80];
    time_t statusmsg_time;
};
struct editorConfig E;

void abufAppend(struct abuf *ab, const char *s, int len) {
    /*
    Appends a string to the buffer
    */
    char *new = realloc(ab->b, ab->len + len);
    memcpy(&new[ab->len], s, len);
    ab->b = new;
    ab->len += len;
}

void abufFree(struct abuf *ab) {
    /*
    Frees the buffer
    */
    free(ab->b);
}

void die(const char *s) {
    /*
    Prints an error message and exits the program
    */
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);
    perror(s);
    exit(1);
}

void disableRawMode() {
    /*
    Disables raw mode by restoring the original terminal state
    */
    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_state) == -1)
        die("tcsetattr");
}

void enableRawMode() {
    /*
    Enables raw mode by turning off the following
    flags in the termios struct:
    - BRKINT: Disables break condition signal
    - ECHO: Echo input characters
    - ICANON: Canonical mode (read input line-by-line)
    - ICRNL: Disables carriage return to newline conversion
    - IEXTEN: Disables Ctrl-V and Ctrl-O
    - INPCK: Disables parity checking
    - ISIG: Disables signals (Ctrl-C, Ctrl-Z)
    - ISTRIP: Disables stripping of 8th bit
    - IXON: Disables software flow control
    - OPOST: Disables output processing
    */
    if (tcgetattr(STDIN_FILENO, &E.orig_state) == -1)
        die("tcgetattr");
    atexit(disableRawMode);

    struct termios raw = E.orig_state;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(OPOST);
    raw.c_cflag |= (CS8);
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 0;
    raw.c_cc[VTIME] = 1;

    if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
        die("tcsetattr");
}

int editorReadKey() {
    /*
    Reads a single keypress from the user and returns it
    */
    int nread;
    char ch;
    while ((nread = read(STDIN_FILENO, &ch, 1)) != 1) {
        if (nread == -1 && errno != EAGAIN)
            die("read");
    }

    if (ch == '\x1b') {
        char seq[3];
        if (read(STDIN_FILENO, &seq[0], 1) != 1) return '\x1b';
        if (read(STDIN_FILENO, &seq[1], 1) != 1) return '\x1b';

        if (seq[0] == '[') {
            if (seq[1] >= '0' && seq[1] <= '9') {
                if (read(STDIN_FILENO, &seq[2], 1) != 1) return '\x1b';
                if (seq[2] == '~') {
                    switch (seq[1]) {
                        case '1': return HOME_KEY;
                        case '3': return DEL_KEY;
                        case '4': return END_KEY;
                        case '5': return PAGE_UP;
                        case '6': return PAGE_DOWN;
                        case '7': return HOME_KEY;
                        case '8': return END_KEY;
                    }
                }
            } else {
                switch (seq[1]) {
                    case 'A': return ARROW_UP;
                    case 'B': return ARROW_DOWN;
                    case 'C': return ARROW_RIGHT;
                    case 'D': return ARROW_LEFT;
                    case 'H': return HOME_KEY;
                    case 'F': return END_KEY;
                }
            }
        } else if (seq[0] == 'O') {
            switch (seq[1]) {
                case 'H': return HOME_KEY;
                case 'F': return END_KEY;
            }
        }
        return '\x1b';
    }
    return ch;
}

int getCursorPosition(int *rows, int *cols) {
    /*
    Gets the cursor position using the '6n' escape sequence
    */
    char buf[32];
    unsigned int i = 0;
    if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
        return -1;

    while (i < sizeof(buf) - 1) {
        if (read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }

    buf[i] = '\0';
    if (buf[0] != '\x1b' || buf[1] != '[') return -1;
    if (sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;
    return 0;
}

int getWindowSize(int *rows, int *cols) {
    /*
    Gets the size of the terminal window by ioctl
    or by moving cursor to the bottom right corner
    and invoking getCursorPosition
    */
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
            return -1;
        return getCursorPosition(rows, cols);
    } else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

int editorRowCxToRx(erow *row, int cx) {
    int rx = 0;
    for (int j = 0; j < cx; j++) {
        if (row->chars[j] == '\t')
        rx += (KAI_TAB_STOP - 1) - (rx % KAI_TAB_STOP);
        rx++;
    }
    return rx;
}

void editorUpdateRow(erow *row) {
    int tabs = 0;
    int j;
    for (j = 0; j < row->size; j++)
        if (row->chars[j] == '\t') tabs++;

    free(row->render);
    row->render = malloc(row->size + tabs * (KAI_TAB_STOP - 1) + 1);

    int idx = 0;
    for (j = 0; j < row->size; j++) {
        if (row->chars[j] == '\t') {
        row->render[idx++] = ' ';
        while (idx % KAI_TAB_STOP != 0) row->render[idx++] = ' ';
        } else {
        row->render[idx++] = row->chars[j];
        }
    }
    row->render[idx] = '\0';
    row->rsize = idx;
}

void editorAppendRow(char *s, size_t len) {
    /*
    Appends a row to the editor
    */
    E.row = realloc(E.row, sizeof(erow) * (E.numrows + 1));
    int at = E.numrows;
    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, s, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rsize = 0;
    E.row[at].render = NULL;
    editorUpdateRow(&E.row[at]);
    E.numrows++;
}

void editorRowInsertChar(erow *row, int at, int c) {
    if (at < 0 || at > row->size) at = row->size;

    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;

    editorUpdateRow(row);
}

void editorInsertChar(int c) {
    if (E.cy == E.numrows) editorAppendRow("", 0);

    editorRowInsertChar(&E.row[E.cy], E.cx, c);
    E.cx++;
}

void editorScroll() {
    E.rx = 0;
    if (E.cy < E.numrows) {
        E.rx = editorRowCxToRx(&E.row[E.cy], E.cx);
    }

    if (E.cy < E.rowoff) {
        E.rowoff = E.cy;
    }
    if (E.cy >= E.rowoff + E.screenrows) {
        E.rowoff = E.cy - E.screenrows + 1;
    }
    if (E.rx < E.coloff) {
        E.coloff = E.rx;
    }
    if (E.rx >= E.coloff + E.screencols) {
        E.coloff = E.rx - E.screencols + 1;
    }
}

void editorDrawRows(struct abuf *ab) {
    /*
    Draws tilda characters at the end of input
    to represent the editor screen,
    draws the welcome message at the center of the screen
    */
    for (int i = 0; i < E.screenrows; i++) {
        int filerow = i + E.rowoff;
        if (filerow >= E.numrows) {
            if (E.numrows == 0 && i == E.screenrows / 3) {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome),
                    "Kai editor -- version %s", KAI_VERSION);
                
                if (welcomelen > E.screencols) welcomelen = E.screencols;

                int padding = (E.screencols - welcomelen) / 2;
                if (padding) {
                    abufAppend(ab, "~", 1);
                    padding--;
                }

                while (padding--) abufAppend(ab, " ", 1);
                abufAppend(ab, welcome, welcomelen);
            } else {
                abufAppend(ab, "~", 1);
            }
        } else {
            int len = E.row[filerow].rsize - E.coloff;
            if (len < 0) len = 0;
            if (len > E.screencols) len = E.screencols;
            abufAppend(ab, &E.row[filerow].render[E.coloff], len);
        }

        abufAppend(ab, "\x1b[K", 3);
        abufAppend(ab, "\r\n", 2);
    }
}

void editorDrawMessageBar(struct abuf *ab) {
    abufAppend(ab, "\x1b[K", 3);

    int msglen = strlen(E.statusmsg);
    if (msglen > E.screencols) msglen = E.screencols;

    if (msglen && time(NULL) - E.statusmsg_time < 5)
        abufAppend(ab, E.statusmsg, msglen);
}

void editorDrawStatusBar(struct abuf *ab) {
    abufAppend(ab, "\x1b[7m", 4);

    char status[80], rstatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines",
        E.filename ? E.filename : "[No Name]", E.numrows);
    int rlen = snprintf(rstatus, sizeof(rstatus), "%d/%d",
        E.cy + 1, E.numrows);
    
    if (len > E.screencols) len = E.screencols;

    abufAppend(ab, status, len);

    while (len < E.screencols) {
        if (E.screencols - len == rlen) {
            abufAppend(ab, rstatus, rlen);
            break;
        } else {
            abufAppend(ab, " ", 1);
            len++;
        }
    }
    abufAppend(ab, "\x1b[m", 3);
    abufAppend(ab, "\r\n", 2);
}

void editorRefreshScreen() {
    /*
    Refreshes the screen by clearing it and redrawing the
    editor contents
    */
    editorScroll();

    struct abuf ab = ABUF_INIT;
    abufAppend(&ab, "\x1b[?25l", 6);
    abufAppend(&ab, "\x1b[H", 3);
    editorDrawRows(&ab);
    editorDrawStatusBar(&ab);
    editorDrawMessageBar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.cy - E.rowoff) + 1,
                                            (E.rx - E.coloff + 1));
    abufAppend(&ab, buf, strlen(buf));

    abufAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, ab.len);
    abufFree(&ab);
}

void editorSetStatusMessage(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}

void editorMoveCursor(int key) {
    /*
    Moves the cursor based on the key pressed,
    e.g. arrow keys or wasd
    */
    erow *row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];

    switch (key) {
        case ARROW_LEFT:
            if (E.cx != 0) {
                E.cx--;
            } else if (E.cy > 0) {
                E.cy--;
                E.cx = E.row[E.cy].size;
            }
            break;
        case ARROW_RIGHT:
            if (row && E.cx < row->size) {
                E.cx++;
            } else if (row && E.cx == row->size) {
                E.cy++;
                E.cx = 0;
            }
            break;
        case ARROW_UP:
            if (E.cy != 0) E.cy--;
            break;
        case ARROW_DOWN:
            if (E.cy < E.numrows) E.cy++;
            break;
    }

    row = (E.cy >= E.numrows) ? NULL : &E.row[E.cy];
    int rowlen = row ? row->size : 0;
    if (E.cx > rowlen) {
        E.cx = rowlen;
    }
}

void editorProcessKeypress() {
    /*
    Processes a keypress from the user
    */
    int ch = editorReadKey();
    switch (ch) {
        case '\r':
            break;
        case CTRL_KEY('q'):
            write(STDOUT_FILENO, "\x1b[2J", 4);
            write(STDOUT_FILENO, "\x1b[H", 3);
            exit(0);
            break;
        case HOME_KEY:
            E.cx = 0;
            break;
        case END_KEY:
            if (E.cy < E.numrows) E.cx = E.row[E.cy].size;
            break;

        case BACKSPACE:
        case CTRL_KEY('h'):
        case DEL_KEY:
            break;
        case PAGE_UP:
        case PAGE_DOWN:
            {
                if (ch == PAGE_UP) {
                    E.cy = E.rowoff;
                } else if (ch == PAGE_DOWN) {
                    E.cy = E.rowoff + E.screenrows - 1;
                    if (E.cy > E.numrows) E.cy = E.numrows;
                }

                int times = E.screenrows;
                while (times--)
                    editorMoveCursor(ch == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
            break;
        case ARROW_DOWN:
        case ARROW_LEFT:
        case ARROW_RIGHT:
        case ARROW_UP:
            editorMoveCursor(ch);
            break;

        case CTRL_KEY('l'):
        case '\x1b':
            break;

        default:
            editorInsertChar(ch);
            break;
    }
}

void initEditor() {
    E.cx = 0, E.cy = 0;
    E.rx = 0;
    E.rowoff = 0, E.coloff = 0;
    E.numrows = 0;
    E.row = NULL;
    E.filename = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_time = 0;

    if (getWindowSize(&E.screenrows, &E.screencols) == -1)
        die("getWindowSize");
    
    E.screenrows -= 2;
}

void editorOpen(char *filename) {
    free(E.filename);
    E.filename = strdup(filename);
    FILE *fp = fopen(filename, "r");
    if (!fp) die("fopen");

    char *line = NULL;
    size_t linecap = 0;
    ssize_t linelen;
    while ((linelen = getline(&line, &linecap, fp)) != -1) {
        while (linelen > 0 && (line[linelen - 1] == '\n' ||
            line[linelen - 1] == '\r'))
            linelen--;

        editorAppendRow(line, linelen);
    }

    free(line);
    fclose(fp);
}

int main(int argc, char *argv[]) {
    enableRawMode();
    initEditor();
    if (argc >= 2) {
        editorOpen(argv[1]);
    }

    editorSetStatusMessage("HELP: Ctrl-Q = quit");

    while (1) {
        editorRefreshScreen();
        editorProcessKeypress();
    }

    return 0;
}
