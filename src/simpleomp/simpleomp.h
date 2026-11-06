#ifndef SIMPLEOMP_H
#define SIMPLEOMP_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int omp_get_max_threads();
void omp_set_num_threads(int num_threads);
int omp_get_dynamic();
void omp_set_dynamic(int dynamic);
int omp_get_num_threads();
int omp_get_thread_num();
int kmp_get_blocktime();
void kmp_set_blocktime(int blocktime);

#ifdef __cplusplus
}
#endif

#endif // SIMPLEOMP_H
