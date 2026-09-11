#ifndef SSU_SCORE_H
#define SSU_SCORE_H

#define MAX_STUDENTS 100
#define MAX_QUESTIONS 100
#define MAX_OPTION_ARGS 5
#define WARNING_PENALTY 0.1
#define EXECUTION_TIMEOUT_SECONDS 5.0
#define SSU_SCORE_OK 0
#define SSU_SCORE_ERROR 1
#define SSU_SCORE_HELP 2

int ssu_score(int argc, char *argv[]);
void print_usage(const char *program_name);

#endif
