#define _DEFAULT_SOURCE
#define _BSD_SOURCE
#define _GNU_SOURCE

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

/*==== DEFINES ====*/ // 163

#define JEDITOR_VERSION "0.0.1"
#define JEDITOR_TAB_STOP 8
#define JEDITOR_QUIT_TIMES 3

#define CTRL_KEY(k) ((k) & 0x1f)

enum editorKey {
    BACKSPACE = 127,
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN,
    DEL_KEY,
    HOME_KEY,
    END_KEY,
    PAGE_UP,
    PAGE_DOWN
};

enum editorHighlight {
    HL_NORMAL = 0,
    HL_COMMENT,
    HL_MCOMMENT,
    HL_KEYWORD1,
    HL_KEYWORD2,
    HL_STRING,
    HL_NUMBER,
    HL_MATCH
};

#define HL_HIGHLIGHT_NUMBERS (1<<0)
#define HL_HIGHLIGHT_STRINGS (1<<1)

/*==== DATA ====*/

struct EditorSyntax{
    char* filetype;
    char** filematch;
    char** keywords;
    char* singleLineCommentStart;
    char* multilineCommentStart;
    char* multilineCommentEnd;
    int flags;
};

typedef struct eRow{
    int idx;
    int size;
    int rndrSize;
    char* chars;
    char* render;
    unsigned char* highlight;
    int hlOpenComment;
} eRow;

struct EditorConfig {
    int curX, curY;
    int rndrX; // For eRow render (tabs and such)
    int terminalRows;
    int terminalCols;
    int rowOff;
    int colOff;
    int numRows;
    eRow* row;
    int dirty;
    char* filename;
    char statusMsg[80];
    time_t statusMsgTime;
    struct EditorSyntax* syntax;
    struct termios originalTermios;
};

struct EditorConfig E;

/*==== FILE TYPES ====*/

char* C_HL_Extentions[] = {".c", ".h", ".cpp", NULL};
char* C_HL_Keywords[] = { // Ending in | = keyword2
    "switch", "if", "while", "for", "break", "continue", "return", "else",
    "struct", "union", "typedef", "static", "enum", "class", "case",
    "int|", "long|", "double|", "float|", "char|", "unsigned|", "signed|",
    "void|", NULL
};

struct EditorSyntax HLDB[] = {
    {
        "c",
        C_HL_Extentions,
        C_HL_Keywords,
        "//", "/*", "*/",
        HL_HIGHLIGHT_NUMBERS | HL_HIGHLIGHT_STRINGS
    },
};

#define HLDB_ENTRIES (sizeof(HLDB)) / sizeof(HLDB[0])

/*==== PROTOTYPES ====*/

void EditorSetStatusMessage(const char* fmt, ...);
void EditorRefreshScreen();
char* EditorPrompt(char* prompt, void(*callback)(char*, int));

/*==== TERMINAL ====*/

void Die(const char* msg){
    write(STDOUT_FILENO, "\x1b[2J", 4);
    write(STDOUT_FILENO, "\x1b[H", 3);

    perror(msg);
    exit(1);
}

void DisableRawMode(){
    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.originalTermios) == -1){
        Die("tcsetattr");
    }
}

void EnableRawMode(){
    if(tcgetattr(STDIN_FILENO, &E.originalTermios) == -1){
        Die("tcgetattr");
    }
    atexit(DisableRawMode);

    struct termios raw = E.originalTermios;
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON); // Input flags to disable special input processing
    raw.c_oflag &= ~(OPOST); // Disable output processing to output raw data
    raw.c_cflag &= ~(CS8); // Control flags configure character size (CS8 = 8 bits per byte)
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG); // Local flags to disable terminal features ie. echo & canonical
    raw.c_cc[VMIN] = 0; // The minimum number of bytes of input before read() can return
    raw.c_cc[VTIME] = 1; // The maximum amount of time before read() returns (1 = 100 milliseconds)

    if(tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1){
        Die("tcsetattr");
    }
}

int EditorReadKey(){
    int nread;
    char c;
    while((nread = read(STDIN_FILENO, &c, 1)) != 1){
        if(nread == -1 && errno != EAGAIN){
            Die("read");
        }
    }

    if (c == '\x1b') {
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
    } else {
        return c;
    }
}

int GetCursorPosition(int* rows, int* cols){
    char buf[32];
    unsigned int i = 0;

    if(write(STDOUT_FILENO, "\x1b[6n", 4) != 4) return -1;

    while(i < sizeof(buf) - 1){
        if(read(STDIN_FILENO, &buf[i], 1) != 1) break;
        if(buf[i] == 'R') break;
        i++;
    }

    buf[i] = '\0';

    if(buf[0] != '\x1b' || buf[1] != '[') return -1;
    if(sscanf(&buf[2], "%d;%d", rows, cols) != 2) return -1;

    return 0;
}

int GetTerminalSize(int* rows, int* cols){
    struct winsize ws;

    if(ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0){
        if(write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12) return -1; // If we cant move cursor to bottom right return (fallback for ioctl)
        return GetCursorPosition(rows, cols);
    }
    else {
        *cols = ws.ws_col;
        *rows = ws.ws_row;
        return 0;
    }
}

/*==== SYNTAX HIGHLIGHTING ====*/

int IsSeparator(int c){
    return isspace(c) || c == '\0' || strchr(",.()+-/*=~%<>[];", c) != NULL;
}

void EditorUpdateSyntax(eRow* row){
    row->highlight = realloc(row->highlight, row->rndrSize);
    memset(row->highlight, HL_NORMAL, row->rndrSize);

    if(E.syntax == NULL) return;

    char** keywords = E.syntax->keywords;

    char* scs = E.syntax->singleLineCommentStart;
    char* mcs = E.syntax->multilineCommentStart;
    char* mce = E.syntax->multilineCommentEnd;

    int scsLen = scs ? strlen(scs) : 0;
    int mcsLen = mcs ? strlen(mcs) : 0;
    int mceLen = mce ? strlen(mce) : 0;

    int prevSep = 1;
    int inString = 0;
    int inComment = (row->idx > 0 && E.row[row->idx - 1].hlOpenComment);

    int i = 0;
    while(i < row->rndrSize){
        char c = row->render[i];
        unsigned char prevHL = (i > 0) ? row->highlight[i - 1] : HL_NORMAL;

        if(scsLen && !inString && !inComment){
            if(!strncmp(&row->render[i], scs, scsLen)){
                memset(&row->highlight[i], HL_COMMENT, row->rndrSize - i);
                break;
            }
        }

        if(mcsLen && mceLen && !inString){
            if(inComment){
                row->highlight[i] = HL_MCOMMENT;
                if(!strncmp(&row->render[i], mce, mceLen)){
                    memset(&row->highlight[i], HL_MCOMMENT, mceLen);
                    i += mceLen;
                    inComment = 0;
                    prevSep = 1;
                    continue;
                }
                else {
                    i++;
                    continue;
                }
            }
            else if(!strncmp(&row->render[i], mcs, mcsLen)){
                memset(&row->highlight[i], HL_MCOMMENT, mcsLen);
                i += mcsLen;
                inComment = 1;
                continue;
            }
        }

        if(E.syntax->flags & HL_HIGHLIGHT_STRINGS){
            if(inString){
                row->highlight[i] = HL_STRING;

                if(c == '\\' && i + 1 < row->rndrSize){
                    row->highlight[i + 1] = HL_STRING;
                    i += 2;
                    continue;
                }

                if(c == inString) inString = 0;

                ++i;
                prevSep = 1;
                continue;
            }
            else {
                if(c == '"' || c == '\''){
                    inString = c;
                    row->highlight[i] = HL_STRING;
                    ++i;
                    continue;
                }
            }
        }

        if(E.syntax->flags & HL_HIGHLIGHT_NUMBERS){
            if((isdigit(c) && (prevSep || prevHL == HL_NUMBER)) || (c == '.' && prevHL == HL_NUMBER)){
                row->highlight[i] = HL_NUMBER;
                i++;
                prevSep = 0;
                continue;
            }
        }

        if(prevSep){
            int j;
            for(j = 0; keywords[j]; ++j){
                int kLen = strlen(keywords[j]);
                int kw2 = keywords[j][kLen - 1] == '|';

                if(kw2) kLen--;

                if(!strncmp(&row->render[i], keywords[j], kLen) && IsSeparator(row->render[i + kLen])){
                    memset(&row->highlight[i], kw2 ? HL_KEYWORD2 : HL_KEYWORD1, kLen);
                    i += kLen;
                    break;
                }
            }
            if(keywords[j] != NULL){
                prevSep = 0;
                continue;
            }
        }

        prevSep = IsSeparator(c);
        ++i;
    }

    int changed = (row->hlOpenComment != inComment);
    row->hlOpenComment = inComment;
    if(changed && row->idx + 1 < E.numRows){
        EditorUpdateSyntax(&E.row[row->idx + 1]);
    }
}

int EditorSyntaxToColor(int hl){
    switch(hl){
    case HL_COMMENT:
    case HL_MCOMMENT: return 36;
    case HL_KEYWORD1: return 33;
    case HL_KEYWORD2: return 32;
    case HL_STRING: return 35;
    case HL_NUMBER: return 31;
    case HL_MATCH: return 34;
    default: return 37;
    }
}

void EditorSelectSyntaxHighlight(){
    E.syntax = NULL;
    if(E.filename == NULL) return;

    char* ext = strrchr(E.filename, '.');

    for(unsigned int j = 0; j < HLDB_ENTRIES; ++j){
        struct EditorSyntax* s = &HLDB[j];

        unsigned int i = 0;
        while(s->filematch[i]){
            int isExt = (s->filematch[i][0] == '.');
            if((isExt && ext && !strcmp(ext, s->filematch[i])) || (!isExt && strstr(E.filename, s->filematch[i]))){
                E.syntax = s;
            }

            int filerow;
            for(filerow = 0; filerow < E.numRows; ++filerow){
                EditorUpdateSyntax(&E.row[filerow]);
            }

            i++;
        }
    }
}

/*==== ROW OPERATIONS ====*/

int EditorRowCurXToRndrX(eRow* row, int curX){
    int rx = 0;
    int j;
    for(j = 0; j < curX; ++j){
        if(row->chars[j] == '\t')
            rx += (JEDITOR_TAB_STOP - 1) - (rx % JEDITOR_TAB_STOP);

        rx++;
    }

    return rx;
}

int EditorRowRendrXToCurX(eRow* row, int rowX){
    int curRx = 0;
    int cx;
    for(cx = 0; cx < row->size; cx++){
        if(row->chars[cx] == '\t')
            curRx += (JEDITOR_TAB_STOP - 1) - (curRx % JEDITOR_TAB_STOP);

        curRx++;

        if(curRx > rowX) return cx; 
    }

    return cx;
}

void EditorUpdateRow(eRow* row){
    int tabs = 0;
    int j;

    for(j = 0; j < row->size; ++j){
        if(row->chars[j] == '\t') 
            tabs++;
    }

    free(row->render);
    row->render = malloc(row->size + tabs * (JEDITOR_TAB_STOP - 1) + 1);

    int idx = 0;
    for(j = 0; j < row->size; ++j){
        if(row->chars[j] == '\t'){
            row->render[idx++] = ' ';
            while(idx % JEDITOR_TAB_STOP != 0) row->render[idx++] = ' ';
        }
        else {
            row->render[idx++] = row->chars[j];
        }
    }

    row->render[idx] = '\0';
    row->rndrSize = idx;

    EditorUpdateSyntax(row);
}

void EditorInsertRow(int at, char* str, size_t len){
    if(at < 0 || at > E.numRows) return;

    E.row = realloc(E.row, sizeof(eRow) * (E.numRows + 1));
    memmove(&E.row[at + 1], &E.row[at], sizeof(eRow) * (E.numRows - at));
    for(int j = at + 1; j <= E.numRows; ++j) E.row[j].idx++;

    E.row[at].idx = at;

    E.row[at].size = len;
    E.row[at].chars = malloc(len + 1);
    memcpy(E.row[at].chars, str, len);
    E.row[at].chars[len] = '\0';

    E.row[at].rndrSize = 0;
    E.row[at].render = NULL;
    E.row[at].highlight = NULL;
    E.row[at].hlOpenComment = 0;
    EditorUpdateRow(&E.row[at]);

    E.numRows++;
    E.dirty++;
}

void EditorFreeRow(eRow* row){
    free(row->render);
    free(row->chars);
    free(row->highlight);
}

void EditorDelRow(int at){
    if(at < 0 || at >= E.numRows) return;

    EditorFreeRow(&E.row[at]);
    memmove(&E.row[at], &E.row[at + 1], sizeof(eRow) * (E.numRows - at - 1));
    for(int j = at; j < E.numRows - 1; ++j) E.row[j].idx--;
    E.numRows--;
    E.dirty++;
}

void EditorRowInsertChar(eRow* row, int at, int c){
    if(at < 0 || at > row->size) at = row->size;

    row->chars = realloc(row->chars, row->size + 2);
    memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
    row->size++;
    row->chars[at] = c;
    EditorUpdateRow(row);

    E.dirty++;
}

void EditorRowAppendString(eRow* row, char* str, size_t len){
    row->chars = realloc(row->chars, row->size + len + 1);
    memcpy(&row->chars[row->size], str, len);
    row->size += len;
    row->chars[row->size] = '\0';
    EditorUpdateRow(row);
    E.dirty++;
}

void EditorRowDelChar(eRow* row, int at){
    if(at < 0 || at >= row->size) return;

    memmove(&row->chars[at], &row->chars[at + 1], row->size - at);
    row->size--;
    EditorUpdateRow(row);
    E.dirty++;
}

/*==== EDITOR OPERATIONS ====*/

void EditorInsertChar(int c){
    if(E.curY == E.numRows){ // If we are on ~
        EditorInsertRow(E.numRows, "", 0);
    }

    EditorRowInsertChar(&E.row[E.curY], E.curX, c);
    E.curX++;
}

void EditorInsertNewline(){
    if(E.curX == 0){
        EditorInsertRow(E.curY, "", 0);
    }
    else {
        eRow* row = &E.row[E.curY];
        EditorInsertRow(E.curY + 1, &row->chars[E.curX], row->size - E.curX);
        row = &E.row[E.curY];
        row->size = E.curX;
        row->chars[row->size] = '\0';
        EditorUpdateRow(row);
    }

    E.curY++;
    E.curX = 0;
}

void EditorDelChar(){
    if(E.curY == E.numRows) return;
    if(E.curX == 0 && E.curY == 0) return;

    eRow* row = &E.row[E.curY];
    if(E.curX > 0){
        EditorRowDelChar(row, E.curX - 1);
        E.curX--;
    }
    else {
        E.curX = E.row[E.curY - 1].size;
        EditorRowAppendString(&E.row[E.curY - 1], row->chars, row->size);
        EditorDelRow(E.curY);
        E.curY--;
    }
}

/*==== FILE I/O ====*/

char* EditorRowsToString(int* bufLen){
    int totalLen = 0;
    int j;
    for(j = 0; j < E.numRows; ++j){
        totalLen += E.row[j].size + 1;
    }

    *bufLen = totalLen;

    char* buf = malloc(totalLen);
    char* ptr = buf;

    for(j = 0; j < E.numRows; ++j){
        memcpy(ptr, E.row[j].chars, E.row[j].size);
        ptr += E.row[j].size;
        *ptr = '\n';
        ptr++;
    }

    return buf;
}

void EditorOpen(char* file){
    free(E.filename);
    E.filename = strdup(file);

    EditorSelectSyntaxHighlight();

    FILE* fptr = fopen(file, "r");
    if(!fptr) Die("fopen");

    char* line = NULL;
    size_t lineCap = 0;
    ssize_t lineLen;

    while((lineLen = getline(&line, &lineCap, fptr)) != -1){
        while(lineLen > 0 && (line[lineLen - 1] == '\n' || line[lineLen - 1] == '\r')){
            lineLen--;
        }
        EditorInsertRow(E.numRows, line, lineLen);
    }

    free(line);
    fclose(fptr);
    E.dirty = 0;
}

void EditorSave(){
    if(E.filename == NULL){
        E.filename = EditorPrompt("Save as: %s", NULL);
        if(E.filename == NULL){
            EditorSetStatusMessage("Save aborted!");
            return;
        }

        EditorSelectSyntaxHighlight();
    }

    int len;
    char* buf = EditorRowsToString(&len);

    int fd = open(E.filename, O_RDWR | O_CREAT, 0644); // 0644 is standard permission for owner to read write
    if(fd != -1){
        if(ftruncate(fd, len) != -1){
            if(write(fd, buf, len) == len){
                close(fd);
                free(buf);

                E.dirty = 0;
                EditorSetStatusMessage("%d bytes written to disk", len);
                return;
            }
        }
        close(fd);
    }

    free(buf);
    EditorSetStatusMessage("Cannot save! I/O error: %s", strerror(errno));
}

/*==== FIND ====*/

void EditorFindCallback(char* query, int key){
    static int lastMatch = -1;
    static int direction = 1;

    static int savedHLLine;
    static char* savedHL = NULL;

    if(savedHL){
        memcpy(E.row[savedHLLine].highlight, savedHL, E.row[savedHLLine].rndrSize);
        free(savedHL);
        savedHL = NULL;
    }

    if(key == '\r' || key == '\x1b'){
        lastMatch = -1;
        direction = 1;
        return;
    }
    else if (key == ARROW_RIGHT || key == ARROW_DOWN){
        direction = 1;
    }
    else if (key == ARROW_LEFT || key == ARROW_UP){
        direction = -1;
    }
    else {
        lastMatch = -1;
        direction = 1;
    }

    if(lastMatch == -1) direction = 1;
    int current = lastMatch;

    int i;
    for(i = 0; i < E.numRows; ++i){
        current += direction;
        if(current == -1) current = E.numRows - 1;
        else if(current == E.numRows) current = 0;

        eRow* row = &E.row[current];
        char* match = strstr(row->render, query);

        if(match){
            lastMatch = current;

            E.curY = current;
            E.curX = EditorRowRendrXToCurX(row, match - row->render);
            E.rowOff = E.numRows;

            savedHLLine = current;
            savedHL = malloc(row->rndrSize);
            memcpy(savedHL, row->highlight, row->rndrSize);
            memset(&row->highlight[match - row->render], HL_MATCH, strlen(query));
            break;
        }
    }
}

void EditorFind(){
    int savedCurX   = E.curX;    
    int savedCurY   = E.curY;
    int savedColOff = E.colOff;
    int savedRowOff = E.rowOff;

    char* query = EditorPrompt("Search: %s (ESC | ARROWS | ENTER)", EditorFindCallback);

    if(query){
        free(query);
    }
    else {
        E.curX = savedCurX;
        E.curY = savedCurY;
        E.colOff = savedColOff;
        E.rowOff = savedRowOff;
    }
}

/*==== APPEND BUFFER ====*/

struct abuf {
    char* buf;
    int len;
};

#define ABUF_INIT {NULL, 0}

void abAppend(struct abuf* ab, const char* str, int len){
    char* new = realloc(ab->buf, ab->len + len);

    if(new == NULL) return;

    memcpy(&new[ab->len], str, len);
    ab->buf = new;
    ab->len += len;
}

void abFree(struct abuf* ab){
    free(ab->buf);
}

/*==== OUTPUT ====*/

void EditorScroll(){
    E.rndrX = 0;
    if(E.curY < E.numRows){
        E.rndrX = EditorRowCurXToRndrX(&E.row[E.curY], E.curX);
    }

    if(E.curY < E.rowOff){
        E.rowOff = E.curY;
    }
    
    if(E.curY >= E.rowOff + E.terminalRows){
        E.rowOff = E.curY - E.terminalRows + 1;
    }

    if(E.rndrX < E.colOff){
        E.colOff = E.rndrX;
    }

    if(E.rndrX >= E.colOff + E.terminalCols){
        E.colOff = E.rndrX - E.terminalCols + 1;
    }
}

void EditorDrawRows(struct abuf *ab) {
    int y;

    for (y = 0; y < E.terminalRows; y++) {
        int fileRow = y + E.rowOff;

        if(fileRow >= E.numRows){
            if (E.numRows == 0 && y == E.terminalRows / 3) {
                char welcome[80];
                int welcomelen = snprintf(welcome, sizeof(welcome), "JEDITOR -- version %s", JEDITOR_VERSION);

                if (welcomelen > E.terminalCols) welcomelen = E.terminalCols;

                int padding = (E.terminalCols - welcomelen) / 2;
                if (padding) {
                    abAppend(ab, "~", 1);
                    padding--;
                }

                while (padding--) abAppend(ab, " ", 1);

                abAppend(ab, welcome, welcomelen);
            }
            else {
                abAppend(ab, "~", 1);
            }
        } else {
            int len = E.row[fileRow].rndrSize - E.colOff;
            if(len < 0) len = 0;
            if(len > E.terminalCols) len = E.terminalCols;
            
            char* c = &E.row[fileRow].render[E.colOff];
            unsigned char* hl = &E.row[fileRow].highlight[E.colOff];
            int curColor = -1;

            int j;
            for(j = 0; j < len; ++j){
                if(iscntrl(c[j])){
                    char sym = (c[j] <= 26) ? '@' + c[j] : '?';
                    abAppend(ab, "\x1b[7m", 4);
                    abAppend(ab, &sym, 3);
                    abAppend(ab, "\x1b[m", 3);
                    
                    if(curColor != -1){
                        char buf[16];
                        int cLen = snprintf(buf, sizeof(buf), "\x1b[%dm", curColor);
                        abAppend(ab, buf, cLen);
                    }
                }
                else if(hl[j] == HL_NORMAL){
                    if(curColor != -1){
                        abAppend(ab, "\x1b[39m", 5);
                        curColor = -1;
                    }
                    abAppend(ab, &c[j], 1);
                }
                else {
                    int color = EditorSyntaxToColor(hl[j]);
                    if(color != curColor){
                        curColor = color;
                        char buf[16];
                        int cLen = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                        abAppend(ab, buf, cLen);
                    }

                    abAppend(ab, &c[j], 1);
                }
            }

            abAppend(ab, "\x1b[39m", 5);
        }

        abAppend(ab, "\x1b[K", 3);
        abAppend(ab, "\r\n", 2);
    }
}

void EditorDrawStatusBar(struct abuf* ab){
    abAppend(ab, "\x1b[7m", 4); // 7m invert color

    char status[80], rStatus[80];
    int len = snprintf(status, sizeof(status), "%.20s - %d lines %s", E.filename ? E.filename : "[No Name]", E.numRows, E.dirty ? "(modified)" : "");

    int rLen = snprintf(rStatus, sizeof(rStatus), "%s | %d/%d", E.syntax ? E.syntax->filetype : "no filetype", E.curY + 1, E.numRows);

    if(len > E.terminalCols) len = E.terminalCols;
    abAppend(ab, status, len);

    while(len < E.terminalCols){
        if(E.terminalCols - len == rLen){
            abAppend(ab, rStatus, rLen);
            break;
        }
        else {
            abAppend(ab, " ", 1);
            len++;
        }
    }

    abAppend(ab, "\x1b[m", 3); // m reset color
    abAppend(ab, "\r\n", 2);
}

void EditorDrawMessageBar(struct abuf* ab){
    abAppend(ab, "\x1b[K", 3);

    int msgLen = strlen(E.statusMsg);

    if(msgLen > E.terminalCols) msgLen = E.terminalCols;
    if(msgLen && time(NULL) - E.statusMsgTime < 5){
        abAppend(ab, E.statusMsg, msgLen);
    }
}

void EditorRefreshScreen(){
    EditorScroll();

    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6); // Hide cursor
    abAppend(&ab, "\x1b[H", 3);

    EditorDrawRows(&ab);
    EditorDrawStatusBar(&ab);
    EditorDrawMessageBar(&ab);

    char buf[32];
    snprintf(buf, sizeof(buf), "\x1b[%d;%dH", (E.curY - E.rowOff) + 1, (E.rndrX - E.colOff) + 1);
    abAppend(&ab, buf, strlen(buf));

    abAppend(&ab, "\x1b[?25h", 6); // Show cursor

    write(STDOUT_FILENO, ab.buf, ab.len);
    abFree(&ab);
}

void EditorSetStatusMessage(const char* fmt, ...){
    va_list ap;
    va_start(ap, fmt);

    vsnprintf(E.statusMsg, sizeof(E.statusMsg), fmt, ap);

    va_end(ap);

    E.statusMsgTime = time(NULL);
}

/*==== INPUT ====*/

char* EditorPrompt(char* prompt, void(*callback)(char*, int)){
    size_t bufSize = 128;
    char* buf = malloc(bufSize);

    size_t bufLen = 0;
    buf[0] = '\0';

    while(1){
        EditorSetStatusMessage(prompt, buf);
        EditorRefreshScreen();

        int c = EditorReadKey();
        if(c == DEL_KEY || c == CTRL_KEY('h') || c == BACKSPACE){
            if(bufLen != 0) buf[--bufLen] = '\0';
        }
        else if(c == '\x1b'){
            EditorSetStatusMessage("");
            if(callback) callback(buf, c);
            free(buf);
            return NULL;
        }
        else if(c == '\r'){
            if(bufLen != 0){
                EditorSetStatusMessage("");
                if(callback) callback(buf, c);
                return buf;
            }
        }
        else if (!iscntrl(c) && c < 128){
            if(bufLen == bufSize - 1){
                bufSize *= 2;
                buf = realloc(buf, bufSize);
            }

            buf[bufLen++] = c;
            buf[bufLen] = '\0';
        }

        if(callback) callback(buf, c);
    }
}

void EditorMoveCursor(int key){
    eRow* row = (E.curY >= E.numRows) ? NULL : &E.row[E.curY];

    switch(key){
    case ARROW_LEFT:
        if(E.curX != 0){
            E.curX--;
        } else if(E.curY > 0){
            E.curY--;
            E.curX = E.row[E.curY].size;
        }
        break;
    case ARROW_RIGHT:
        if(row && E.curX < row->size){
            E.curX++;
        } else if(row && E.curX == row->size){
            E.curY++;
            E.curX = 0;
        }
        break;
    case ARROW_UP:
        if(E.curY != 0){
            E.curY--;
        }
        break;
    case ARROW_DOWN:
        if(E.curY < E.numRows){
            E.curY++;
        }
        break;
    }

    row = (E.curY >= E.numRows) ? NULL : &E.row[E.curY];
    int rowLen = row ? row->size : 0;
    if(E.curX > rowLen){
        E.curX = rowLen;
    }
}

void EditorProcessKeypress(){
    static int quitTimes = JEDITOR_QUIT_TIMES;

    int c = EditorReadKey();

    switch(c){
    case '\r':
        EditorInsertNewline();
        break;
    case CTRL_KEY('q'):
        if(E.dirty && quitTimes > 0){
            EditorSetStatusMessage("WARNING: Unsaved Changes. Press Ctrl-Q %d times to force quit", quitTimes);
            quitTimes--;
            return;
        }

        write(STDOUT_FILENO, "\x1b[2J", 4);
        write(STDOUT_FILENO, "\x1b[H", 3);
        exit(0);
        break;
    case CTRL_KEY('s'):
        EditorSave();
        break;
    case HOME_KEY:
        E.curX = 0;
        break;
    case END_KEY:
        if(E.curY < E.numRows)
            E.curX = E.row[E.curY].size;
        break;
    case CTRL_KEY('f'):
        EditorFind();
        break;
    case BACKSPACE:
    case CTRL_KEY('h'):
    case DEL_KEY:
        if(c == DEL_KEY) EditorMoveCursor(ARROW_RIGHT);
        EditorDelChar();
        break;
    case PAGE_UP:
    case PAGE_DOWN:
        {
            if(c == PAGE_UP){
                E.curY = E.rowOff;
            } else if (c == PAGE_DOWN){
                E.curY = E.rowOff + E.terminalRows - 1;
                if(E.curY > E.numRows) E.curY = E.numRows;
            }

            int times = E.terminalRows;
            while(times--){
                EditorMoveCursor(c == PAGE_UP ? ARROW_UP : ARROW_DOWN);
            }
        }
        break;
    case ARROW_UP:
    case ARROW_DOWN:
    case ARROW_LEFT:
    case ARROW_RIGHT:
        EditorMoveCursor(c);
        break;
    case CTRL_KEY('l'):
    case '\x1b':
        break;
    default:
        EditorInsertChar(c);
        break;
    }

    quitTimes = JEDITOR_QUIT_TIMES;
}

/*==== INIT ====*/

void InitEditor(){
    E.curX    = 0;
    E.curY    = 0;
    E.rndrX   = 0;
    E.rowOff  = 0;
    E.colOff  = 0;
    E.numRows = 0;
    E.dirty   = 0;
    E.row      = NULL;
    E.filename = NULL;
    E.statusMsg[0] = '\0';
    E.statusMsgTime = 0;
    E.syntax = NULL;

    if(GetTerminalSize(&E.terminalRows, &E.terminalCols) == -1){
        Die("GetTerminalSize");
    }

    E.terminalRows -= 2;
}

int main(int argc, char* argv[]){
    EnableRawMode();
    InitEditor();

    if(argc >= 2){
        EditorOpen(argv[1]);
    }

    EditorSetStatusMessage("HELP: Ctrl-S: SAVE | Ctrl-Q: QUIT | CTRL-F: FIND");

    while(1){
        EditorRefreshScreen();
        EditorProcessKeypress();
    }

    return 0;
}