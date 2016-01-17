/* 
 * trans.c - Matrix transpose B = A^T
 *
 * Each transpose function must have a prototype of the form:
 * void trans(int M, int N, int A[N][M], int B[M][N]);
 *
 * A transpose function is evaluated by counting the number of misses
 * on a 1KB direct mapped cache with a block size of 32 bytes.
 */ 
#include <stdio.h>
#include "cachelab.h"
#include "contracts.h"

int is_transpose(int M, int N, int A[N][M], int B[M][N]);

/* 
 * transpose_submit - This is the solution transpose function that you
 *     will be graded on for Part B of the assignment. Do not change
 *     the description string "Transpose submission", as the driver
 *     searches for that string to identify the transpose function to
 *     be graded. The REQUIRES and ENSURES from 15-122 are included
 *     for your convenience. They can be removed if you like.
 */
char transpose_submit_desc[] = "Transpose submission";
void transpose_submit(int M, int N, int A[N][M], int B[M][N]) {
/* Description:
 *   Implements a cache-optimized transpose operation for the three test cases.
 *   A series of if statements is used to tailor the function for each case.
 * Cases:
 *   32x32:
 *     This cases uses blocking to reduce the misses by using two outer loops
 *     to cycle over the blocks and two inner loops to cycle within the blocks.
 *     If the col/row indices are on the diagonal, A_ij is temporarily stored
 *     to reduce the number of references to B. The block size was tuned and
 *     8 was determined to be the optimal size.
 *   64x64:
 *     This case was much more difficult to figure out than 32x32. It uses
 *     a block size of 8, like before, which we desire because a single 
 *     eight element row can be held in a cache line. The code than does
 *     a bunch of convoluted memory copies and local variable swaps to try 
 *     and maximize the amount of row-wise accesses to both A and B. 
 *     This improved the miss rate from ~1700 with the simple blocking scheme 
 *     to 1331.
 *   61x67:
 *     This cases uses the same scheme as 32x32, but includes extra logic in
 *     the inner for loops to account for blocks on the edges being smaller
 *     than block_size. The optimal block size was found to be 16. 
 */
    REQUIRES(M > 0);
    REQUIRES(N > 0);

    int temp;
    int diag_index;

    if (N == 32) {
      // Transpose with blocking to reduce misses
      int block_size = 8;
      for (int i = 0; i < N; i += block_size) {
        for (int j = 0; j < M; j += block_size) {
          for (int block_row = 0; block_row < block_size; block_row++) {
            for (int block_col = 0; block_col < block_size; block_col++) {
               /* assign simple transpose operation, except if diagonal 
                * we can move the value of A outside inner loop to reduce 
                * accesses to B
                */
               if (i+block_row == j+block_col) {
                 temp = A[i+block_row][j+block_col];
                 diag_index = i+block_row;
               } else {
                 B[j+block_col][i+block_row] = A[i+block_row][j+block_col];  
               }
            }
            if (i == j) {
              B[diag_index][diag_index] = temp;
            }
          }
        }
      }
    }
    if (N == 64) {
      // Transpose with blocking to reduce misses
      int block_size = 8;
      int sub_size = 4;
      for (int block_row = 0; block_row < N; block_row += block_size) {
        for (int block_col = 0; block_col < M; block_col += block_size) {
          for (int k = block_row; k < block_row + sub_size; k++) {
            // Transpose top-left block of A normally
            B[block_col + 0][k] = A[k][block_col + 0];
            B[block_col + 1][k] = A[k][block_col + 1];
            B[block_col + 2][k] = A[k][block_col + 2];
            B[block_col + 3][k] = A[k][block_col + 3];
            
            // copy top-right block of A to top-right block of B 
            // (it is equal to the bottom-left block of B)
            B[block_col + 0][k+4] = A[k][block_col + 4];
            B[block_col + 1][k+4] = A[k][block_col + 5];
            B[block_col + 2][k+4] = A[k][block_col + 6];
            B[block_col + 3][k+4] = A[k][block_col + 7];
          }
          for (int k = block_col; k < block_col + sub_size; k++) {
            // scan top-right block of B to local variables
            int temp1 = B[k][block_row + 4];
            int temp2 = B[k][block_row + 5];
            int temp3 = B[k][block_row + 6];
            int temp4 = B[k][block_row + 7];
            
            // Find the actual top-right block of B in A
            B[k][block_row + 4] = A[block_row + 4][k];
            B[k][block_row + 5] = A[block_row + 5][k];
            B[k][block_row + 6] = A[block_row + 6][k];
            B[k][block_row + 7] = A[block_row + 7][k];

            // Copy temp to the bottom-left block of B
            B[k+4][block_row + 0] = temp1;
            B[k+4][block_row + 1] = temp2;
            B[k+4][block_row + 2] = temp3;
            B[k+4][block_row + 3] = temp4;

            // transpose the bottom-right block of B normally
            B[k+4][block_row+4] = A[block_row+4][k+4];     
            B[k+4][block_row+5] = A[block_row+5][k+4];     
            B[k+4][block_row+6] = A[block_row+6][k+4];     
            B[k+4][block_row+7] = A[block_row+7][k+4];     
          } 
        } 
      }
    }
    if (N == 67) {
      // Transpose with blocking to reduce misses
      int block_size = 16;
      for (int i = 0; i < N; i += block_size) {
        for (int j = 0; j < M; j += block_size) {
          for (int row = i; (row < i + block_size) && (row < N); row++) {
            for (int col = j; (col < j + block_size) && (col < M); col++) {
               if (row == col) {
                 temp = A[row][col];
                 diag_index = row;
               } else {
                 B[col][row] = A[row][col];  
               }
            }
            if (i == j) {
              B[diag_index][diag_index] = temp;
            }
          }
        }
      }
    }

    ENSURES(is_transpose(M, N, A, B));
}

/* 
 * You can define additional transpose functions below. We've defined
 * a simple one below to help you get started. 
 */ 

/* 
 * trans - A simple baseline transpose function, not optimized for the cache.
 */
char trans_desc[] = "Simple row-wise scan transpose";
void trans(int M, int N, int A[N][M], int B[M][N])
{
    int i, j, tmp;

    REQUIRES(M > 0);
    REQUIRES(N > 0);

    for (i = 0; i < N; i++) {
        for (j = 0; j < M; j++) {
            tmp = A[i][j];
            B[j][i] = tmp;
        }
    }    

    ENSURES(is_transpose(M, N, A, B));
}

/*
 * registerFunctions - This function registers your transpose
 *     functions with the driver.  At runtime, the driver will
 *     evaluate each of the registered functions and summarize their
 *     performance. This is a handy way to experiment with different
 *     transpose strategies.
 */
void registerFunctions()
{
    /* Register your solution function */
    registerTransFunction(transpose_submit, transpose_submit_desc); 

    /* Register any additional transpose functions */
    registerTransFunction(trans, trans_desc); 

}

/* 
 * is_transpose - This helper function checks if B is the transpose of
 *     A. You can check the correctness of your transpose by calling
 *     it before returning from the transpose function.
 */
int is_transpose(int M, int N, int A[N][M], int B[M][N])
{
    int i, j;

    for (i = 0; i < N; i++) {
        for (j = 0; j < M; ++j) {
            if (A[i][j] != B[j][i]) {
                return 0;
            }
        }
    }
    return 1;
}

