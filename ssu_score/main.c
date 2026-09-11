#include <stdio.h>
#include <sys/time.h>

#include "ssu_score.h"

#define USEC_PER_SEC 1000000L

int main(int argc, char *argv[])
{
    struct timeval begin;
    struct timeval end;
    long sec;
    long usec;
    int status;

    gettimeofday(&begin, NULL);
    status = ssu_score(argc, argv);
    gettimeofday(&end, NULL);

    if (status == SSU_SCORE_OK) {
        sec = end.tv_sec - begin.tv_sec;
        usec = end.tv_usec - begin.tv_usec;
        if (usec < 0) {
            --sec;
            usec += USEC_PER_SEC;
        }

        printf("Runtime: %ld:%06ld(sec:usec)\n", sec, usec);
    }
    return status == SSU_SCORE_HELP ? 0 : status;
}
