/*
  patest_pink.c

  generate Pink Noise using Gardner method.
  Optimization suggested by James McCartney uses a tree
  to select which random value to replace.

  x x x x x x x x x x x x x x x x 
  x   x   x   x   x   x   x   x   
  x       x       x       x       
  x               x               
  x                               

  Tree is generated by counting trailing zeros in an increasing index.
  When the index is zero, no random number is selected.

  This program uses the Portable Audio library which is under development.
  For more information see:   http://www.audiomulch.com/portaudio/

  Author: Phil Burk, http://www.softsynth.com
	
  Revision History:

  Copyleft 1999 Phil Burk - No rights reserved.
*/

#include <stdio.h>
#include <math.h>
#include "pink.h"

/************************************************************/
/* Calculate pseudo-random 32 bit number based on linear congruential method. */
static unsigned long generate_random_number( void )
{
    static unsigned long rand_seed = 22222;  /* Change this for different random sequences. */
    rand_seed = (rand_seed * 196314165) + 907633515;
    return rand_seed;
}

/* Setup PinkNoise structure for N rows of generators. */
void initialize_pink_noise( pink_noise_t *pink, int num_rows )
{
    int i;
    long pmax;
    pink->pink_index = 0;
    pink->pink_index_mask = (1<<num_rows) - 1;
/* Calculate maximum possible signed random value. Extra 1 for white noise always added. */
    pmax = (num_rows + 1) * (1<<(PINK_RANDOM_BITS-1));
    pink->pink_scalar = 1.0F / pmax;
/* Initialize rows. */
    for( i=0; i<num_rows; i++ ) pink->pink_rows[i] = 0;
    pink->pink_running_sum = 0;
}

/* generate Pink noise values between -1.0 and +1.0 */
float generate_pink_noise_sample( pink_noise_t *pink )
{
    long new_random;
    long sum;
    float output;

/* Increment and mask index. */
    pink->pink_index = (pink->pink_index + 1) & pink->pink_index_mask;

/* If index is zero, don't update any random values. */
    if( pink->pink_index != 0 )
    {
	/* Determine how many trailing zeros in PinkIndex. */
	/* This algorithm will hang if n==0 so test first. */
	int num_zeros = 0;
	int n = pink->pink_index;
	while( (n & 1) == 0 )
	{
	    n = n >> 1;
	    num_zeros++;
	}

	/* Replace the indexed ROWS random value.
	 * Subtract and add back to Running_sum instead of adding all the random
	 * values together. Only one changes each time.
	 */
	pink->pink_running_sum -= pink->pink_rows[num_zeros];
	new_random = ((long)generate_random_number()) >> PINK_RANDOM_SHIFT;
	pink->pink_running_sum += new_random;
	pink->pink_rows[num_zeros] = new_random;
    }
	
/* Add extra white noise value. */
    new_random = ((long)generate_random_number()) >> PINK_RANDOM_SHIFT;
    sum = pink->pink_running_sum + new_random;

/* Scale to range of -1.0 to 0.9999. */
    output = pink->pink_scalar * sum;

    return output;
}
