#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <spawn.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "blank.h"
#include "ssu_score.h"

extern char **environ;

/*
 * Questions, students, and each student's wrong answers are intentionally
 * represented as linked lists: the assignment requires list-based file
 * management, and -s must reorder the result list itself before it is saved.
 */
typedef enum {
    QUESTION_BLANK,
    QUESTION_PROGRAM
} QuestionType;

typedef struct Question {
    char *name;
    char *stem;
    char *answer_path;
    char *answer_executable;
    char *answer_output;
    QuestionType type;
    double score;
    bool answer_ready;
    struct Question *next;
} Question;

typedef struct WrongAnswer {
    const Question *question;
    struct WrongAnswer *next;
} WrongAnswer;

typedef struct Student {
    char *id;
    char *directory;
    double scores[MAX_QUESTIONS];
    double total;
    WrongAnswer *wrong_head;
    WrongAnswer *wrong_tail;
    struct Student *next;
} Student;

typedef struct {
    bool modify_scores;
    bool print_scores;
    bool print_wrong;
    bool thread_all;
    bool sort_enabled;
    bool error_enabled;
    const char *output_name;
    const char *error_name;
    const char *sort_key;
    int sort_direction;
    const char *display_ids[MAX_OPTION_ARGS];
    size_t display_count;
    const char *thread_questions[MAX_OPTION_ARGS];
    size_t thread_count;
} Options;

typedef struct {
    char *name;
    QuestionType type;
} QuestionEntry;

static char *student_root;
static char *answer_root;
static Question *question_head;
static Question *questions[MAX_QUESTIONS];
static size_t question_count;
static Student *student_head;
static Student *students[MAX_STUDENTS];
static size_t student_count;
static Options options;
static char output_path[PATH_MAX];
static char error_path[PATH_MAX];

static void free_all(void);
static int parse_options(int argc, char *argv[]);
static int resolve_directory(const char *input, char **result);
static int discover_questions(void);
static int discover_students(void);
static int load_or_create_score_table(void);
static int modify_score_table(void);
static int write_score_table(void);
static int validate_option_targets(void);
static int prepare_error_directory(void);
static int prepare_answer_programs(void);
static int grade_students(void);
static int write_result_csv(void);
static Student *sort_student_list(Student *head);
static bool selected_for_display(const Student *student);
static void print_wrong_answers(const Student *student);

static void print_error(const char *format, const char *value)
{
    fprintf(stderr, "ssu_score: ");
    fprintf(stderr, format, value);
    fputc('\n', stderr);
}

static char *duplicate_string(const char *text)
{
    char *copy = malloc(strlen(text) + 1);
    if (copy != NULL)
        strcpy(copy, text);
    return copy;
}

static int path_join(char *buffer, size_t size, const char *left,
                     const char *right)
{
    int length = snprintf(buffer, size, "%s/%s", left, right);
    if (length < 0 || (size_t)length >= size) {
        print_error("path is too long: %s", right);
        return -1;
    }
    return 0;
}

static bool has_suffix(const char *text, const char *suffix)
{
    size_t text_length = strlen(text);
    size_t suffix_length = strlen(suffix);
    return text_length >= suffix_length &&
           strcmp(text + text_length - suffix_length, suffix) == 0;
}

static bool invalid_csv_field(const char *text)
{
    return strchr(text, ',') != NULL || strchr(text, '\n') != NULL ||
           strchr(text, '\r') != NULL;
}

void print_usage(const char *program_name)
{
    printf("Usage: %s <STD_DIR> <ANS_DIR> [OPTION]\n", program_name);
    puts("Options:");
    puts("  -n <CSVFILENAME>       save results to the specified .csv file");
    puts("  -m                     modify scores in score_table.csv");
    puts("  -c [STUDENTIDS ...]    print totals (all students when omitted)");
    puts("  -p [STUDENTIDS ...]    print wrong problems (all when omitted)");
    puts("  -t [QNAMES ...]        link selected programs with -lpthread");
    puts("                         (all program questions when omitted)");
    puts("  -s <stdid|score> <1|-1> sort rows by ID or total score");
    puts("  -e <DIRNAME>           save compiler diagnostics by student");
    puts("  -h                     print this help only");
}

int ssu_score(int argc, char *argv[])
{
    int status = 1;

    memset(&options, 0, sizeof(options));

    if (argc == 2 && strcmp(argv[1], "-h") == 0) {
        print_usage(argv[0]);
        return SSU_SCORE_HELP;
    }
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-h") == 0) {
            print_error("-h cannot be used with other arguments: %s", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (parse_options(argc, argv) < 0 ||
        resolve_directory(argv[1], &student_root) < 0 ||
        resolve_directory(argv[2], &answer_root) < 0 ||
        discover_questions() < 0 || discover_students() < 0 ||
        validate_option_targets() < 0 ||
        load_or_create_score_table() < 0)
        goto done;

    if (options.modify_scores && modify_score_table() < 0)
        goto done;
    if (options.error_enabled && prepare_error_directory() < 0)
        goto done;
    if (prepare_answer_programs() < 0 || grade_students() < 0)
        goto done;

    if (options.sort_enabled) {
        /* The linked list itself is reordered before CSV output. */
        student_head = sort_student_list(student_head);
    }
    if (write_result_csv() < 0)
        goto done;

    printf("result saved.. (%s)\n", output_path);
    if (options.error_enabled)
        printf("error saved.. (%s/)\n", error_path);
    status = 0;

done:
    free_all();
    return status;
}

static bool is_option_name(const char *text)
{
    return strcmp(text, "-n") == 0 || strcmp(text, "-m") == 0 ||
           strcmp(text, "-c") == 0 || strcmp(text, "-p") == 0 ||
           strcmp(text, "-t") == 0 || strcmp(text, "-s") == 0 ||
           strcmp(text, "-e") == 0 || strcmp(text, "-h") == 0;
}

static int add_limited_arguments(const char *option_name, int argc, char *argv[],
                                 int *index, const char **destination,
                                 size_t *destination_count, bool shared)
{
    size_t local_count = 0;
    int cursor = *index + 1;
    bool overflow_printed = false;

    while (cursor < argc && !is_option_name(argv[cursor])) {
        if (shared && *destination_count > 0) {
            print_error("student IDs may be supplied after -c/-p only once: %s",
                        argv[cursor]);
            return -1;
        }
        if (local_count < MAX_OPTION_ARGS) {
            destination[local_count] = argv[cursor];
        } else {
            if (!overflow_printed) {
                printf("Maximum Number of Argument Exceeded. ::");
                overflow_printed = true;
            }
            printf(" %s", argv[cursor]);
        }
        ++local_count;
        ++cursor;
    }
    if (overflow_printed)
        putchar('\n');

    if (shared) {
        if (local_count > MAX_OPTION_ARGS)
            local_count = MAX_OPTION_ARGS;
        if (*destination_count == 0) {
            for (size_t i = 0; i < local_count; ++i)
                options.display_ids[i] = destination[i];
            *destination_count = local_count;
        }
    } else {
        *destination_count = local_count > MAX_OPTION_ARGS ?
                             MAX_OPTION_ARGS : local_count;
    }

    *index = cursor - 1;
    (void)option_name;
    return 0;
}

static int require_arguments(int argc, int index, int count,
                             const char *option_name)
{
    if (index + count >= argc) {
        print_error("missing argument for option %s", option_name);
        return -1;
    }
    return 0;
}

static int parse_options(int argc, char *argv[])
{
    bool seen_n = false, seen_m = false, seen_t = false;
    bool seen_s = false, seen_e = false;

    /* A custom scanner is used because -c, -p, and -t take optional lists. */
    for (int i = 3; i < argc; ++i) {
        if (strcmp(argv[i], "-n") == 0) {
            if (seen_n || require_arguments(argc, i, 1, "-n") < 0)
                return -1;
            options.output_name = argv[++i];
            seen_n = true;
        } else if (strcmp(argv[i], "-m") == 0) {
            if (seen_m) {
                print_error("duplicate option: %s", argv[i]);
                return -1;
            }
            options.modify_scores = true;
            seen_m = true;
        } else if (strcmp(argv[i], "-c") == 0 ||
                   strcmp(argv[i], "-p") == 0) {
            const char *collected[MAX_OPTION_ARGS] = {0};
            size_t count_before = options.display_count;
            bool *flag = strcmp(argv[i], "-c") == 0 ?
                         &options.print_scores : &options.print_wrong;
            if (*flag) {
                print_error("duplicate option: %s", argv[i]);
                return -1;
            }
            *flag = true;
            if (add_limited_arguments(argv[i], argc, argv, &i, collected,
                                      &options.display_count, true) < 0)
                return -1;
            (void)count_before;
        } else if (strcmp(argv[i], "-t") == 0) {
            if (seen_t) {
                print_error("duplicate option: %s", argv[i]);
                return -1;
            }
            seen_t = true;
            if (add_limited_arguments("-t", argc, argv, &i,
                                      options.thread_questions,
                                      &options.thread_count, false) < 0)
                return -1;
            options.thread_all = options.thread_count == 0;
        } else if (strcmp(argv[i], "-s") == 0) {
            if (seen_s || require_arguments(argc, i, 2, "-s") < 0)
                return -1;
            options.sort_key = argv[++i];
            if (strcmp(options.sort_key, "stdid") != 0 &&
                strcmp(options.sort_key, "score") != 0) {
                print_error("invalid -s key: %s", options.sort_key);
                return -1;
            }
            if (strcmp(argv[i + 1], "1") != 0 && strcmp(argv[i + 1], "-1") != 0) {
                print_error("-s direction must be 1 or -1: %s", argv[i + 1]);
                return -1;
            }
            options.sort_direction = atoi(argv[++i]);
            options.sort_enabled = true;
            seen_s = true;
        } else if (strcmp(argv[i], "-e") == 0) {
            if (seen_e || require_arguments(argc, i, 1, "-e") < 0)
                return -1;
            options.error_name = argv[++i];
            options.error_enabled = true;
            seen_e = true;
        } else {
            print_error("unknown option or misplaced argument: %s", argv[i]);
            return -1;
        }
    }

    if (options.output_name != NULL && !has_suffix(options.output_name, ".csv")) {
        print_error("result filename must end in .csv: %s", options.output_name);
        return -1;
    }
    return 0;
}

static int resolve_directory(const char *input, char **result)
{
    char resolved[PATH_MAX];
    struct stat metadata;

    if (realpath(input, resolved) == NULL) {
        print_error("directory does not exist: %s", input);
        return -1;
    }
    if (stat(resolved, &metadata) < 0 || !S_ISDIR(metadata.st_mode)) {
        print_error("not a directory: %s", input);
        return -1;
    }
    *result = duplicate_string(resolved);
    if (*result == NULL) {
        print_error("out of memory while resolving: %s", input);
        return -1;
    }
    return 0;
}

static void question_numbers(const char *name, long *major, long *minor)
{
    char *end;
    *major = strtol(name, &end, 10);
    *minor = 0;
    if (*end == '-')
        *minor = strtol(end + 1, NULL, 10);
}

static int compare_question_entry(const void *left_pointer,
                                  const void *right_pointer)
{
    const QuestionEntry *left = left_pointer;
    const QuestionEntry *right = right_pointer;
    long left_major, left_minor, right_major, right_minor;
    question_numbers(left->name, &left_major, &left_minor);
    question_numbers(right->name, &right_major, &right_minor);
    if (left_major != right_major)
        return left_major < right_major ? -1 : 1;
    if (left_minor != right_minor)
        return left_minor < right_minor ? -1 : 1;
    return strcmp(left->name, right->name);
}

static char *question_stem(const char *name)
{
    const char *dot = strrchr(name, '.');
    size_t length = dot == NULL ? strlen(name) : (size_t)(dot - name);
    char *stem = malloc(length + 1);
    if (stem == NULL)
        return NULL;
    memcpy(stem, name, length);
    stem[length] = '\0';
    return stem;
}

static int discover_questions(void)
{
    DIR *directory = opendir(answer_root);
    struct dirent *entry;
    QuestionEntry found[MAX_QUESTIONS];
    size_t count = 0;

    if (directory == NULL) {
        print_error("cannot open answer directory: %s", answer_root);
        return -1;
    }
    /* Only regular .txt/.c files are questions; generated files are ignored. */
    while ((entry = readdir(directory)) != NULL) {
        QuestionType type;
        char path[PATH_MAX];
        struct stat metadata;
        if (entry->d_name[0] == '.')
            continue;
        if (has_suffix(entry->d_name, ".txt"))
            type = QUESTION_BLANK;
        else if (has_suffix(entry->d_name, ".c"))
            type = QUESTION_PROGRAM;
        else
            continue;
        if (invalid_csv_field(entry->d_name)) {
            print_error("question name cannot contain CSV separators: %s", entry->d_name);
            for (size_t i = 0; i < count; ++i)
                free(found[i].name);
            closedir(directory);
            return -1;
        }
        if (path_join(path, sizeof(path), answer_root, entry->d_name) < 0 ||
            stat(path, &metadata) < 0 || !S_ISREG(metadata.st_mode))
            continue;
        if (count == MAX_QUESTIONS) {
            print_error("more than 100 questions found in: %s", answer_root);
            for (size_t i = 0; i < count; ++i)
                free(found[i].name);
            closedir(directory);
            return -1;
        }
        found[count].name = duplicate_string(entry->d_name);
        found[count].type = type;
        if (found[count].name == NULL) {
            for (size_t i = 0; i < count; ++i)
                free(found[i].name);
            closedir(directory);
            return -1;
        }
        ++count;
    }
    closedir(directory);
    if (count == 0) {
        print_error("no .txt or .c questions found in: %s", answer_root);
        return -1;
    }
    qsort(found, count, sizeof(found[0]), compare_question_entry);

    Question *tail = NULL;
    for (size_t i = 0; i < count; ++i) {
        Question *question = calloc(1, sizeof(*question));
        char path[PATH_MAX];
        if (question == NULL) {
            for (size_t j = i; j < count; ++j)
                free(found[j].name);
            return -1;
        }
        question->name = found[i].name;
        found[i].name = NULL;
        question->stem = question_stem(question->name);
        question->type = found[i].type;
        if (question->stem == NULL ||
            path_join(path, sizeof(path), answer_root, question->name) < 0) {
            free(question->name);
            free(question->stem);
            free(question);
            for (size_t j = i + 1; j < count; ++j)
                free(found[j].name);
            return -1;
        }
        question->answer_path = duplicate_string(path);
        if (question->answer_path == NULL) {
            free(question->name);
            free(question->stem);
            free(question);
            for (size_t j = i + 1; j < count; ++j)
                free(found[j].name);
            return -1;
        }
        if (tail == NULL)
            question_head = question;
        else
            tail->next = question;
        tail = question;
        questions[question_count++] = question;
    }
    return 0;
}

static int compare_string_pointer(const void *left, const void *right)
{
    const char *const *a = left;
    const char *const *b = right;
    return strcmp(*a, *b);
}

static int discover_students(void)
{
    DIR *directory = opendir(student_root);
    struct dirent *entry;
    char *ids[MAX_STUDENTS];
    size_t count = 0;

    if (directory == NULL) {
        print_error("cannot open student directory: %s", student_root);
        return -1;
    }
    while ((entry = readdir(directory)) != NULL) {
        char path[PATH_MAX];
        struct stat metadata;
        if (entry->d_name[0] == '.')
            continue;
        if (path_join(path, sizeof(path), student_root, entry->d_name) < 0 ||
            stat(path, &metadata) < 0 || !S_ISDIR(metadata.st_mode))
            continue;
        if (invalid_csv_field(entry->d_name)) {
            print_error("student ID cannot contain CSV separators: %s", entry->d_name);
            for (size_t i = 0; i < count; ++i)
                free(ids[i]);
            closedir(directory);
            return -1;
        }
        if (count == MAX_STUDENTS) {
            print_error("more than 100 student directories found in: %s", student_root);
            for (size_t i = 0; i < count; ++i)
                free(ids[i]);
            closedir(directory);
            return -1;
        }
        ids[count] = duplicate_string(entry->d_name);
        if (ids[count] == NULL) {
            for (size_t i = 0; i < count; ++i)
                free(ids[i]);
            closedir(directory);
            return -1;
        }
        ++count;
    }
    closedir(directory);
    qsort(ids, count, sizeof(ids[0]), compare_string_pointer);

    Student *tail = NULL;
    for (size_t i = 0; i < count; ++i) {
        Student *student = calloc(1, sizeof(*student));
        char path[PATH_MAX];
        if (student == NULL) {
            for (size_t j = i; j < count; ++j)
                free(ids[j]);
            return -1;
        }
        student->id = ids[i];
        ids[i] = NULL;
        if (path_join(path, sizeof(path), student_root, student->id) < 0) {
            free(student->id);
            free(student);
            for (size_t j = i + 1; j < count; ++j)
                free(ids[j]);
            return -1;
        }
        student->directory = duplicate_string(path);
        if (student->directory == NULL) {
            free(student->id);
            free(student);
            for (size_t j = i + 1; j < count; ++j)
                free(ids[j]);
            return -1;
        }
        if (tail == NULL)
            student_head = student;
        else
            tail->next = student;
        tail = student;
        students[student_count++] = student;
    }
    if (student_count == 0) {
        print_error("no student directories found in: %s", student_root);
        return -1;
    }
    return 0;
}

static Question *find_question(const char *name)
{
    for (Question *question = question_head; question != NULL; question = question->next) {
        if (strcmp(question->name, name) == 0 || strcmp(question->stem, name) == 0)
            return question;
    }
    return NULL;
}

static Student *find_student(const char *id)
{
    for (Student *student = student_head; student != NULL; student = student->next) {
        if (strcmp(student->id, id) == 0)
            return student;
    }
    return NULL;
}

static int validate_option_targets(void)
{
    for (size_t i = 0; i < options.display_count; ++i) {
        if (find_student(options.display_ids[i]) == NULL) {
            print_error("student does not exist: %s", options.display_ids[i]);
            return -1;
        }
    }
    for (size_t i = 0; i < options.thread_count; ++i) {
        Question *question = find_question(options.thread_questions[i]);
        if (question == NULL || question->type != QUESTION_PROGRAM) {
            print_error("program question does not exist: %s",
                        options.thread_questions[i]);
            return -1;
        }
    }
    return 0;
}

static char *trim(char *text)
{
    char *end;
    while (isspace((unsigned char)*text))
        ++text;
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1]))
        --end;
    *end = '\0';
    return text;
}

static int parse_nonnegative_double(const char *text, double *value)
{
    char *end;
    errno = 0;
    *value = strtod(text, &end);
    while (isspace((unsigned char)*end))
        ++end;
    if (errno != 0 || end == text || *end != '\0' || *value < 0.0)
        return -1;
    return 0;
}

static int score_table_path(char *path, size_t size)
{
    return path_join(path, size, answer_root, "score_table.csv");
}

static int read_score_table(const char *path)
{
    FILE *file = fopen(path, "r");
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    bool assigned[MAX_QUESTIONS] = {false};

    if (file == NULL) {
        print_error("cannot open score table: %s", path);
        return -1;
    }
    while ((length = getline(&line, &capacity, file)) >= 0) {
        char *comma;
        char *name;
        char *score_text;
        Question *question;
        double score;
        (void)length;
        name = trim(line);
        if (*name == '\0')
            continue;
        comma = strchr(name, ',');
        if (comma == NULL || strchr(comma + 1, ',') != NULL) {
            print_error("invalid score table row: %s", name);
            free(line);
            fclose(file);
            return -1;
        }
        *comma = '\0';
        score_text = trim(comma + 1);
        name = trim(name);
        question = find_question(name);
        if (question == NULL || strcmp(question->name, name) != 0 ||
            parse_nonnegative_double(score_text, &score) < 0) {
            print_error("invalid score table entry: %s", name);
            free(line);
            fclose(file);
            return -1;
        }
        size_t index = 0;
        while (index < question_count && questions[index] != question)
            ++index;
        if (index == question_count || assigned[index]) {
            print_error("duplicate score table entry: %s", name);
            free(line);
            fclose(file);
            return -1;
        }
        question->score = score;
        assigned[index] = true;
    }
    free(line);
    fclose(file);
    for (size_t i = 0; i < question_count; ++i) {
        if (!assigned[i]) {
            print_error("score table is missing question: %s", questions[i]->name);
            return -1;
        }
    }
    return 0;
}

static int prompt_line(const char *prompt, char **line, size_t *capacity)
{
    fputs(prompt, stdout);
    fflush(stdout);
    if (getline(line, capacity, stdin) < 0) {
        print_error("input ended while reading: %s", prompt);
        return -1;
    }
    return 0;
}

static int create_score_table(void)
{
    char *line = NULL;
    size_t capacity = 0;
    long type;
    char *end;
    double blank_score = 0.0, program_score = 0.0;

    puts("score_table.csv file doesn't exist in ANS_DIR!");
    puts("1. input one score for blank questions and one for programs");
    puts("2. input every question's score");
    for (;;) {
        if (prompt_line("select type >> ", &line, &capacity) < 0)
            goto fail;
        errno = 0;
        type = strtol(trim(line), &end, 10);
        if (errno == 0 && (*trim(end) == '\0') && (type == 1 || type == 2))
            break;
        puts("not correct number!");
    }

    /* Type 1 assigns one common score to each of the two question classes. */
    if (type == 1) {
        for (;;) {
            if (prompt_line("Input value of blank question : ", &line, &capacity) < 0)
                goto fail;
            if (parse_nonnegative_double(trim(line), &blank_score) == 0)
                break;
            puts("Input a non-negative number.");
        }
        for (;;) {
            if (prompt_line("Input value of program question : ", &line, &capacity) < 0)
                goto fail;
            if (parse_nonnegative_double(trim(line), &program_score) == 0)
                break;
            puts("Input a non-negative number.");
        }
    }

    for (Question *question = question_head; question != NULL; question = question->next) {
        if (type == 1) {
            question->score = question->type == QUESTION_BLANK ?
                              blank_score : program_score;
        } else {
            char prompt[PATH_MAX];
            snprintf(prompt, sizeof(prompt), "Input of %s: ", question->name);
            for (;;) {
                if (prompt_line(prompt, &line, &capacity) < 0)
                    goto fail;
                if (parse_nonnegative_double(trim(line), &question->score) == 0)
                    break;
                puts("Input a non-negative number.");
            }
        }
    }
    free(line);
    return write_score_table();

fail:
    free(line);
    return -1;
}

static int write_score_table(void)
{
    char path[PATH_MAX];
    FILE *file;
    if (score_table_path(path, sizeof(path)) < 0)
        return -1;
    file = fopen(path, "w");
    if (file == NULL) {
        print_error("cannot write score table: %s", path);
        return -1;
    }
    for (Question *question = question_head; question != NULL; question = question->next)
        fprintf(file, "%s,%.2f\n", question->name, question->score);
    if (fclose(file) != 0) {
        print_error("cannot finish score table: %s", path);
        return -1;
    }
    return 0;
}

static int load_or_create_score_table(void)
{
    char path[PATH_MAX];
    if (score_table_path(path, sizeof(path)) < 0)
        return -1;
    if (access(path, F_OK) == 0)
        return read_score_table(path);
    if (options.modify_scores) {
        print_error("-m requires an existing score table: %s", path);
        return -1;
    }
    return create_score_table();
}

static int modify_score_table(void)
{
    char *line = NULL;
    size_t capacity = 0;
    for (;;) {
        Question *question;
        double score;
        if (prompt_line("Input question's number to modify >> ", &line, &capacity) < 0)
            goto fail;
        char *name = trim(line);
        if (strcmp(name, "no") == 0)
            break;
        question = find_question(name);
        if (question == NULL) {
            printf("%s doesn't exist!\n", name);
            continue;
        }
        printf("Current score : %.2f\n", question->score);
        for (;;) {
            if (prompt_line("New score : ", &line, &capacity) < 0)
                goto fail;
            if (parse_nonnegative_double(trim(line), &score) == 0)
                break;
            puts("Input a non-negative number.");
        }
        question->score = score;
    }
    free(line);
    return write_score_table();
fail:
    free(line);
    return -1;
}

static int resolve_output_path(void)
{
    char candidate[PATH_MAX];
    const char *name = options.output_name;
    if (name == NULL)
        return path_join(output_path, sizeof(output_path), answer_root, "score.csv");
    if (name[0] == '/') {
        if (snprintf(candidate, sizeof(candidate), "%s", name) >= (int)sizeof(candidate))
            return -1;
    } else {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) == NULL ||
            path_join(candidate, sizeof(candidate), cwd, name) < 0)
            return -1;
    }
    char copy[PATH_MAX];
    char parent[PATH_MAX];
    char resolved_parent[PATH_MAX];
    char *slash;
    strcpy(copy, candidate);
    slash = strrchr(copy, '/');
    if (slash == NULL)
        return -1;
    *slash = '\0';
    strcpy(parent, copy[0] == '\0' ? "/" : copy);
    if (realpath(parent, resolved_parent) == NULL) {
        print_error("output parent directory does not exist: %s", parent);
        return -1;
    }
    return path_join(output_path, sizeof(output_path), resolved_parent, slash + 1);
}

static bool selected_for_thread(const Question *question)
{
    if (!options.thread_all && options.thread_count == 0)
        return false;
    if (options.thread_all)
        return question->type == QUESTION_PROGRAM;
    for (size_t i = 0; i < options.thread_count; ++i) {
        if (strcmp(question->name, options.thread_questions[i]) == 0 ||
            strcmp(question->stem, options.thread_questions[i]) == 0)
            return true;
    }
    return false;
}

static int spawn_and_wait(char *const arguments[], const char *stdout_name,
                          const char *stderr_name, double timeout,
                          bool search_path, bool *timed_out)
{
    posix_spawn_file_actions_t actions;
    pid_t child;
    int spawn_status;
    int wait_status;
    struct timespec start;

    *timed_out = false;
    /* File actions replace shell redirection without using a command shell. */
    if (posix_spawn_file_actions_init(&actions) != 0)
        return -1;
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, stdout_name,
                                     O_WRONLY | O_CREAT | O_TRUNC, 0666);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, stderr_name,
                                     O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (search_path)
        spawn_status = posix_spawnp(&child, arguments[0], &actions, NULL,
                                    arguments, environ);
    else
        spawn_status = posix_spawn(&child, arguments[0], &actions, NULL,
                                   arguments, environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawn_status != 0) {
        errno = spawn_status;
        return -1;
    }

    if (timeout <= 0.0) {
        while (waitpid(child, &wait_status, 0) < 0) {
            if (errno != EINTR)
                return -1;
        }
    } else {
        struct timespec pause_time = {0, 10000000L};
        clock_gettime(CLOCK_MONOTONIC, &start);
        /* Poll with a monotonic clock so wall-clock adjustments cannot extend it. */
        for (;;) {
            pid_t result = waitpid(child, &wait_status, WNOHANG);
            if (result == child)
                break;
            if (result < 0 && errno != EINTR)
                return -1;
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double elapsed = (double)(now.tv_sec - start.tv_sec) +
                             (double)(now.tv_nsec - start.tv_nsec) / 1000000000.0;
            if (elapsed >= timeout) {
                kill(child, SIGKILL);
                while (waitpid(child, &wait_status, 0) < 0 && errno == EINTR)
                    ;
                *timed_out = true;
                return 0;
            }
            nanosleep(&pause_time, NULL);
        }
    }
    if (WIFEXITED(wait_status))
        return WEXITSTATUS(wait_status);
    return 128;
}

static int count_warnings(const char *path)
{
    FILE *file = fopen(path, "r");
    char *line = NULL;
    size_t capacity = 0;
    int count = 0;
    if (file == NULL)
        return 0;
    while (getline(&line, &capacity, file) >= 0) {
        char *cursor = line;
        while ((cursor = strstr(cursor, "warning:")) != NULL) {
            ++count;
            cursor += strlen("warning:");
        }
    }
    free(line);
    fclose(file);
    return count;
}

static int compile_source(const char *source, const char *executable,
                          const char *diagnostics, bool pthread_link,
                          int *warnings)
{
    char *arguments[7];
    bool timed_out;
    int index = 0;
    int status;
    arguments[index++] = "gcc";
    arguments[index++] = (char *)source;
    arguments[index++] = "-o";
    arguments[index++] = (char *)executable;
    if (pthread_link)
        arguments[index++] = "-lpthread";
    arguments[index] = NULL;
    status = spawn_and_wait(arguments, "/dev/null", diagnostics, 0.0, true,
                            &timed_out);
    *warnings = count_warnings(diagnostics);
    return status;
}

static int run_program(const char *executable, const char *output, bool *timed_out)
{
    char *arguments[] = {(char *)executable, NULL};
    return spawn_and_wait(arguments, output, "/dev/null",
                          EXECUTION_TIMEOUT_SECONDS, false, timed_out);
}

static int prepare_answer_programs(void)
{
    for (Question *question = question_head; question != NULL; question = question->next) {
        char executable[PATH_MAX];
        char output[PATH_MAX];
        char diagnostic[PATH_MAX];
        int warnings;
        int status;
        bool timed_out;
        if (question->type != QUESTION_PROGRAM)
            continue;
        snprintf(executable, sizeof(executable), "%s/%s.exe", answer_root, question->stem);
        snprintf(output, sizeof(output), "%s/%s.stdout", answer_root, question->stem);
        snprintf(diagnostic, sizeof(diagnostic), "%s/.%s_answer_error.txt",
                 answer_root, question->stem);
        status = compile_source(question->answer_path, executable, diagnostic,
                                selected_for_thread(question), &warnings);
        unlink(diagnostic);
        if (status != 0) {
            print_error("answer program failed to compile: %s", question->name);
            return -1;
        }
        status = run_program(executable, output, &timed_out);
        if (status < 0 || timed_out) {
            print_error("answer program failed or exceeded 5 seconds: %s",
                        question->name);
            return -1;
        }
        question->answer_executable = duplicate_string(executable);
        question->answer_output = duplicate_string(output);
        if (question->answer_executable == NULL || question->answer_output == NULL)
            return -1;
        question->answer_ready = true;
    }
    return 0;
}

static char *read_entire_file(const char *path)
{
    FILE *file = fopen(path, "rb");
    char *buffer;
    long length;
    if (file == NULL)
        return NULL;
    if (fseek(file, 0, SEEK_END) != 0 || (length = ftell(file)) < 0 ||
        fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return NULL;
    }
    buffer = malloc((size_t)length + 1);
    if (buffer == NULL) {
        fclose(file);
        return NULL;
    }
    if (fread(buffer, 1, (size_t)length, file) != (size_t)length) {
        free(buffer);
        fclose(file);
        return NULL;
    }
    buffer[length] = '\0';
    fclose(file);
    return buffer;
}

static bool parse_blank_tree(char *answer, node **root)
{
    char tokens[TOKEN_CNT][MINLEN];
    int index = 0;
    if (*answer == '\0' || !check_brackets(answer) || !make_tokens(answer, tokens))
        return false;
    *root = make_tree(NULL, tokens, &index, 0);
    return *root != NULL;
}

static bool blank_answer_matches(const char *student_path, const char *answer_path)
{
    char *student_file = read_entire_file(student_path);
    char *answer_file = read_entire_file(answer_path);
    char *student_answer;
    char *save_pointer = NULL;
    node *student_tree = NULL;
    bool student_semicolon;
    bool matched = false;

    if (student_file == NULL || answer_file == NULL)
        goto done;
    student_answer = trim(student_file);
    char *colon = strchr(student_answer, ':');
    if (colon != NULL)
        *colon = '\0';
    student_answer = trim(student_answer);
    if (*student_answer == '\0')
        goto done;
    size_t student_length = strlen(student_answer);
    student_semicolon = student_answer[student_length - 1] == ';';
    if (student_semicolon)
        student_answer[student_length - 1] = '\0';
    if (!parse_blank_tree(student_answer, &student_tree))
        goto done;

    /* Every colon-delimited answer candidate gets its own syntax tree. */
    for (char *candidate = strtok_r(answer_file, ":", &save_pointer);
         candidate != NULL;
         candidate = strtok_r(NULL, ":", &save_pointer)) {
        node *answer_tree = NULL;
        int result = true;
        candidate = trim(candidate);
        size_t length = strlen(candidate);
        if (length == 0)
            continue;
        bool answer_semicolon = candidate[length - 1] == ';';
        if (answer_semicolon != student_semicolon)
            continue;
        if (answer_semicolon)
            candidate[length - 1] = '\0';
        if (!parse_blank_tree(candidate, &answer_tree))
            continue;
        compare_tree(student_tree, answer_tree, &result);
        free_node(answer_tree);
        if (result) {
            matched = true;
            break;
        }
    }

done:
    if (student_tree != NULL)
        free_node(student_tree);
    free(student_file);
    free(answer_file);
    return matched;
}

static int next_significant(FILE *file)
{
    int character;
    while ((character = fgetc(file)) != EOF) {
        if (!isspace((unsigned char)character))
            return tolower((unsigned char)character);
    }
    return EOF;
}

static bool output_matches(const char *student_output, const char *answer_output)
{
    FILE *student = fopen(student_output, "rb");
    FILE *answer = fopen(answer_output, "rb");
    bool equal = false;
    if (student == NULL || answer == NULL)
        goto done;
    for (;;) {
        int left = next_significant(student);
        int right = next_significant(answer);
        if (left != right)
            break;
        if (left == EOF) {
            equal = true;
            break;
        }
    }
done:
    if (student != NULL)
        fclose(student);
    if (answer != NULL)
        fclose(answer);
    return equal;
}

static int ensure_directory(const char *path)
{
    char copy[PATH_MAX];
    if (strlen(path) >= sizeof(copy))
        return -1;
    strcpy(copy, path);
    for (char *cursor = copy + 1; *cursor != '\0'; ++cursor) {
        if (*cursor == '/') {
            *cursor = '\0';
            if (mkdir(copy, 0755) < 0 && errno != EEXIST)
                return -1;
            *cursor = '/';
        }
    }
    if (mkdir(copy, 0755) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

static int remove_tree_entry(const char *path)
{
    struct stat metadata;
    if (lstat(path, &metadata) < 0)
        return -1;
    if (!S_ISDIR(metadata.st_mode) || S_ISLNK(metadata.st_mode))
        return unlink(path);
    DIR *directory = opendir(path);
    struct dirent *entry;
    if (directory == NULL)
        return -1;
    while ((entry = readdir(directory)) != NULL) {
        char child[PATH_MAX];
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        if (path_join(child, sizeof(child), path, entry->d_name) < 0 ||
            remove_tree_entry(child) < 0) {
            closedir(directory);
            return -1;
        }
    }
    closedir(directory);
    return rmdir(path);
}

static int canonical_target(const char *input, char *result, size_t size)
{
    char absolute[PATH_MAX];
    char copy[PATH_MAX];
    char parent[PATH_MAX];
    char resolved_parent[PATH_MAX];
    char *slash;
    if (input[0] == '/') {
        if (snprintf(absolute, sizeof(absolute), "%s", input) >= (int)sizeof(absolute))
            return -1;
    } else {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) == NULL ||
            path_join(absolute, sizeof(absolute), cwd, input) < 0)
            return -1;
    }
    strcpy(copy, absolute);
    slash = strrchr(copy, '/');
    if (slash == NULL || strcmp(slash + 1, ".") == 0 ||
        strcmp(slash + 1, "..") == 0 || slash[1] == '\0')
        return -1;
    *slash = '\0';
    strcpy(parent, copy[0] == '\0' ? "/" : copy);
    if (realpath(parent, resolved_parent) == NULL)
        return -1;
    return path_join(result, size, resolved_parent, slash + 1);
}

static int prepare_error_directory(void)
{
    char cwd[PATH_MAX];
    struct stat metadata;
    if (canonical_target(options.error_name, error_path, sizeof(error_path)) < 0) {
        print_error("invalid error directory path: %s", options.error_name);
        return -1;
    }
    if (getcwd(cwd, sizeof(cwd)) == NULL || strcmp(error_path, "/") == 0 ||
        strcmp(error_path, cwd) == 0 || strcmp(error_path, student_root) == 0 ||
        strcmp(error_path, answer_root) == 0) {
        print_error("unsafe error directory path: %s", error_path);
        return -1;
    }
    if (lstat(error_path, &metadata) == 0) {
        if (!S_ISDIR(metadata.st_mode) || S_ISLNK(metadata.st_mode)) {
            print_error("error path is not a directory: %s", error_path);
            return -1;
        }
        DIR *directory = opendir(error_path);
        struct dirent *entry;
        if (directory == NULL)
            return -1;
        while ((entry = readdir(directory)) != NULL) {
            char child[PATH_MAX];
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
                continue;
            if (path_join(child, sizeof(child), error_path, entry->d_name) < 0 ||
                remove_tree_entry(child) < 0) {
                closedir(directory);
                return -1;
            }
        }
        closedir(directory);
    } else if (errno == ENOENT) {
        if (ensure_directory(error_path) < 0) {
            print_error("cannot create error directory: %s", error_path);
            return -1;
        }
    } else {
        return -1;
    }
    return 0;
}

static int copy_file(const char *source, const char *destination)
{
    int input = open(source, O_RDONLY);
    int output;
    char buffer[8192];
    ssize_t length;
    if (input < 0)
        return -1;
    output = open(destination, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (output < 0) {
        close(input);
        return -1;
    }
    while ((length = read(input, buffer, sizeof(buffer))) > 0) {
        ssize_t offset = 0;
        while (offset < length) {
            ssize_t written = write(output, buffer + offset, (size_t)(length - offset));
            if (written < 0) {
                close(input);
                close(output);
                return -1;
            }
            offset += written;
        }
    }
    close(input);
    if (close(output) < 0 || length < 0)
        return -1;
    return 0;
}

static int preserve_diagnostic(const Student *student, const Question *question,
                               const char *diagnostic)
{
    struct stat metadata;
    if (stat(diagnostic, &metadata) < 0)
        return -1;
    if (metadata.st_size == 0) {
        unlink(diagnostic);
        return 0;
    }
    if (!options.error_enabled) {
        unlink(diagnostic);
        return 0;
    }
    char directory[PATH_MAX];
    char destination[PATH_MAX];
    char filename[PATH_MAX];
    if (path_join(directory, sizeof(directory), error_path, student->id) < 0 ||
        ensure_directory(directory) < 0)
        return -1;
    snprintf(filename, sizeof(filename), "%s_error.txt", question->stem);
    if (path_join(destination, sizeof(destination), directory, filename) < 0 ||
        copy_file(diagnostic, destination) < 0)
        return -1;
    unlink(diagnostic);
    return 0;
}

static void add_wrong(Student *student, const Question *question)
{
    WrongAnswer *wrong = calloc(1, sizeof(*wrong));
    if (wrong == NULL)
        return;
    wrong->question = question;
    if (student->wrong_tail == NULL)
        student->wrong_head = wrong;
    else
        student->wrong_tail->next = wrong;
    student->wrong_tail = wrong;
}

static double grade_program(Student *student, const Question *question,
                            const char *source, bool *correct)
{
    char executable[PATH_MAX];
    char output[PATH_MAX];
    char diagnostic[PATH_MAX];
    int warnings = 0;
    int status;
    bool timed_out;
    snprintf(executable, sizeof(executable), "%s/%s.stdexe",
             student->directory, question->stem);
    snprintf(output, sizeof(output), "%s/%s.stdout",
             student->directory, question->stem);
    snprintf(diagnostic, sizeof(diagnostic), "%s/%s_error.txt",
             student->directory, question->stem);
    status = compile_source(source, executable, diagnostic,
                            selected_for_thread(question), &warnings);
    if (preserve_diagnostic(student, question, diagnostic) < 0) {
        print_error("cannot preserve compiler diagnostics for: %s", student->id);
        *correct = false;
        return 0.0;
    }
    if (status != 0) {
        *correct = false;
        return 0.0;
    }
    status = run_program(executable, output, &timed_out);
    if (status < 0 || timed_out ||
        !output_matches(output, question->answer_output)) {
        *correct = false;
        return 0.0;
    }
    *correct = true;
    return question->score - (double)warnings * WARNING_PENALTY;
}

static int grade_students(void)
{
    double selected_sum = 0.0;
    size_t selected_count = 0;
    puts("grading student's test papers..");
    for (Student *student = student_head; student != NULL; student = student->next) {
        size_t index = 0;
        /* Missing or non-regular submissions remain incorrect with zero points. */
        for (Question *question = question_head; question != NULL;
             question = question->next, ++index) {
            char submission[PATH_MAX];
            struct stat metadata;
            bool correct = false;
            double earned = 0.0;
            if (path_join(submission, sizeof(submission), student->directory,
                          question->name) < 0)
                return -1;
            if (stat(submission, &metadata) == 0 && S_ISREG(metadata.st_mode)) {
                if (question->type == QUESTION_BLANK) {
                    correct = blank_answer_matches(submission, question->answer_path);
                    earned = correct ? question->score : 0.0;
                } else {
                    earned = grade_program(student, question, submission, &correct);
                }
            }
            student->scores[index] = earned;
            student->total += earned;
            if (!correct)
                add_wrong(student, question);
        }
        printf("%s is finished..", student->id);
        if (selected_for_display(student) &&
            (options.print_scores || options.print_wrong)) {
            if (options.print_scores)
                printf(" score : %.2f", student->total);
            if (options.print_wrong) {
                if (options.print_scores)
                    printf(", wrong problem : ");
                else
                    printf(" wrong problem : ");
                print_wrong_answers(student);
            }
            selected_sum += student->total;
            ++selected_count;
        }
        putchar('\n');
    }
    if (options.print_scores && selected_count > 0)
        printf("Total average : %.2f\n", selected_sum / (double)selected_count);
    return 0;
}

static int compare_students(const Student *left, const Student *right)
{
    int comparison;
    if (strcmp(options.sort_key, "score") == 0) {
        if (left->total < right->total)
            comparison = -1;
        else if (left->total > right->total)
            comparison = 1;
        else
            comparison = strcmp(left->id, right->id);
    } else {
        comparison = strcmp(left->id, right->id);
    }
    return options.sort_direction == 1 ? comparison : -comparison;
}

static Student *merge_students(Student *left, Student *right)
{
    Student dummy = {0};
    Student *tail = &dummy;
    while (left != NULL && right != NULL) {
        if (compare_students(left, right) <= 0) {
            tail->next = left;
            left = left->next;
        } else {
            tail->next = right;
            right = right->next;
        }
        tail = tail->next;
    }
    tail->next = left != NULL ? left : right;
    return dummy.next;
}

static Student *sort_student_list(Student *head)
{
    Student *slow;
    Student *fast;
    Student *right;
    if (head == NULL || head->next == NULL)
        return head;
    /* Split and merge the linked list in O(n log n) time. */
    slow = head;
    fast = head->next;
    while (fast != NULL && fast->next != NULL) {
        slow = slow->next;
        fast = fast->next->next;
    }
    right = slow->next;
    slow->next = NULL;
    return merge_students(sort_student_list(head), sort_student_list(right));
}

static int write_result_csv(void)
{
    FILE *file;
    if (resolve_output_path() < 0)
        return -1;
    file = fopen(output_path, "w");
    if (file == NULL) {
        print_error("cannot write result file: %s", output_path);
        return -1;
    }
    fputc(',', file);
    for (Question *question = question_head; question != NULL; question = question->next)
        fprintf(file, "%s,", question->name);
    fputs("sum\n", file);
    for (Student *student = student_head; student != NULL; student = student->next) {
        fprintf(file, "%s,", student->id);
        for (size_t i = 0; i < question_count; ++i)
            fprintf(file, "%.2f,", student->scores[i]);
        fprintf(file, "%.2f\n", student->total);
    }
    if (fclose(file) != 0) {
        print_error("cannot finish result file: %s", output_path);
        return -1;
    }
    return 0;
}

static bool selected_for_display(const Student *student)
{
    if (options.display_count == 0)
        return true;
    for (size_t i = 0; i < options.display_count; ++i) {
        if (strcmp(student->id, options.display_ids[i]) == 0)
            return true;
    }
    return false;
}

static void print_wrong_answers(const Student *student)
{
    bool first = true;
    for (WrongAnswer *wrong = student->wrong_head; wrong != NULL; wrong = wrong->next) {
        printf("%s%s(%.2f)", first ? "" : ", ", wrong->question->stem,
               wrong->question->score);
        first = false;
    }
    if (first)
        printf("none");
}

static void free_all(void)
{
    while (question_head != NULL) {
        Question *next = question_head->next;
        free(question_head->name);
        free(question_head->stem);
        free(question_head->answer_path);
        free(question_head->answer_executable);
        free(question_head->answer_output);
        free(question_head);
        question_head = next;
    }
    while (student_head != NULL) {
        Student *next = student_head->next;
        WrongAnswer *wrong = student_head->wrong_head;
        while (wrong != NULL) {
            WrongAnswer *wrong_next = wrong->next;
            free(wrong);
            wrong = wrong_next;
        }
        free(student_head->id);
        free(student_head->directory);
        free(student_head);
        student_head = next;
    }
    free(student_root);
    free(answer_root);
    student_root = NULL;
    answer_root = NULL;
    question_count = 0;
    student_count = 0;
}
