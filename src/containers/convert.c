#include <stdio.h>
#include "containers.h"
#include "convert.h"
#include "util.h"

// file contains grubby stuff that must know impl. details of all container types.

bitset_container_t *bitset_container_from_array( array_container_t *a) {
  bitset_container_t *ans = bitset_container_create();
  int limit = array_container_cardinality(a);
    for (int i = 0; i < limit; ++i) bitset_container_set(ans, a->array[i]);
  return ans;
}

bitset_container_t *bitset_container_from_run( run_container_t *arr) {
    int card = run_container_cardinality(arr);
    bitset_container_t *answer = bitset_container_create();
    for (int rlepos = 0; rlepos < arr->n_runs; ++rlepos) {
        rle16_t vl = arr->runs[rlepos];
    	bitset_set_range(answer->array, vl.value, vl.value + vl.length + 1);
    }
    answer->cardinality = card;
    return answer;
}

array_container_t *array_container_from_bitset( bitset_container_t *bits) {
    array_container_t *result =
        array_container_create_given_capacity(bits->cardinality);
  result->cardinality = bits->cardinality;
  int outpos = 0;
  uint16_t * out = result->array;
  for (int i = 0; i < BITSET_CONTAINER_SIZE_IN_WORDS;  ++i) {
    uint64_t w = bits->array[i];
    while (w != 0) {
      uint64_t t = w & -w;
      int r = __builtin_ctzl(w);
      out[outpos++] = i * 64 + r;
      w ^= t;
    }
  }
  return result;
}


  /**
   * Convert the runcontainer to either a Bitmap or an Array Container, depending
   * on the cardinality.  Frees the container.
   * Allocates and returns new container, which caller is reponsible for disposing
   */

void *convert_to_bitset_or_array_container( run_container_t *r, int32_t card, uint8_t *resulttype) {
    if (card <= DEFAULT_MAX_SIZE) {
      array_container_t *answer = array_container_create_given_capacity(card);
      answer->cardinality=0;
      
      for (int rlepos=0; rlepos < r->n_runs; ++rlepos) {
        uint16_t run_start = r->runs[rlepos].value;
        uint16_t  run_end = run_start + r->runs[rlepos].length;

        //printf("run [%d %d]\n",(int) run_start, (int) run_end);

        for (uint16_t run_value = run_start; run_value <= run_end; ++run_value) {
          answer->array[answer->cardinality++] = run_value;
        }
      }
      assert(card == answer->cardinality);
      *resulttype = ARRAY_CONTAINER_TYPE_CODE;
      run_container_free(r);
      return answer;
    }
    bitset_container_t *answer = bitset_container_create();
    for (int rlepos=0; rlepos < r->n_runs; ++rlepos) {
      uint16_t run_start = r->runs[rlepos].value;
      uint16_t  run_end = run_start + r->runs[rlepos].length;
      bitset_set_range(answer->array, run_start, run_end+1); 
    }
    answer->cardinality = card;
    *resulttype = BITSET_CONTAINER_TYPE_CODE;
    run_container_free(r);
    return answer;
}

/* assumes that container has adequate space.  Run from [s,e] (inclusive) */
static void add_run(run_container_t *r, int s, int e) {
  r->runs[r->n_runs].value = s;
  r->runs[r->n_runs].length = e-s;
  r->n_runs++;
}


/* converts a run container to either an array or a bitset, IF it saves space */
/* If a conversion occurs, the original containers is freed and a new one allocated */

void *convert_run_to_efficient_container(run_container_t *c, uint8_t *typecode_after) {
    int32_t size_as_run_container = run_container_serialized_size_in_bytes(c->n_runs);
    int32_t size_as_bitset_container = bitset_container_serialized_size_in_bytes();
    int32_t card = run_container_cardinality(c);
    int32_t size_as_array_container = array_container_serialized_size_in_bytes(card);
    int32_t min_size_non_run = size_as_bitset_container < size_as_array_container ?
      size_as_bitset_container : size_as_array_container;
    if(size_as_run_container <= min_size_non_run) { // no conversion
      *typecode_after = RUN_CONTAINER_TYPE_CODE;
      return c;
    }
    if(card <= DEFAULT_MAX_SIZE) {
      // to array
      array_container_t *answer = array_container_create(card);
      answer->cardinality=0;
      for (int rlepos=0; rlepos < c->n_runs; ++rlepos) {
        int run_start = c->runs[rlepos].value;
        int run_end = run_start + c->runs[rlepos].length;
         
        for (int run_value = run_start; run_value <= run_end; ++run_value) {
          answer->array[answer->cardinality++] = (uint16_t) run_value;
        }
      }
      *typecode_after = ARRAY_CONTAINER_TYPE_CODE;
      run_container_free(c);
      return answer;
    }
    // else to bitset
    bitset_container_t *answer = bitset_container_create();

    for (int rlepos=0; rlepos < c->n_runs; ++rlepos) {
      int start = c->runs[rlepos].value;
      int end =  start + c->runs[rlepos].length;
      bitset_container_set_range(answer, start, end+1);
    }
    answer->cardinality = card;
    *typecode_after = BITSET_CONTAINER_TYPE_CODE;
    run_container_free(c);
    return answer;
}

/* once converted, the original container is disposed here, rather than
   in roaring_array
*/

// TODO: split into run-  array-  and bitset-  subfunctions for sanity;
// a few function calls won't really matter.

void *convert_run_optimize(void *c, uint8_t typecode_original, uint8_t *typecode_after) {
  if (typecode_original == RUN_CONTAINER_TYPE_CODE) {
    return convert_run_to_efficient_container(c, typecode_after);
  }
  else if (typecode_original == ARRAY_CONTAINER_TYPE_CODE) {
    // it might need to be converted to a run container. 
    array_container_t *c_qua_array = (array_container_t *) c;
    int32_t n_runs = array_container_number_of_runs( c);
    int32_t size_as_run_container = run_container_serialized_size_in_bytes(n_runs);
    int32_t card = array_container_cardinality(c);
    int32_t size_as_array_container = array_container_serialized_size_in_bytes(card);

    if(size_as_run_container >= size_as_array_container) {
      *typecode_after = ARRAY_CONTAINER_TYPE_CODE;
      return c;
    }
    // else convert array to run container
    run_container_t *answer = run_container_create_given_capacity(n_runs);
    int prev=-2;
    int run_start=-1;

    assert(card > 0);
    for (int i=0; i < card; ++i) {
      uint16_t cur_val = c_qua_array->array[i];
      if (cur_val != prev+1) {
        // new run starts; flush old one, if any
        if (run_start != -1)
          add_run(answer, run_start, prev);
        run_start = cur_val;
      }
      prev = c_qua_array->array[i];
    }
    assert(run_start >= 0);
    // now prev is the last seen value
    add_run(answer, run_start, prev); 
    *typecode_after = RUN_CONTAINER_TYPE_CODE;
    array_container_free(c);
    return answer;
  }
  else {  // run conversions on bitset
    // does bitset need conversion to run?
    bitset_container_t *c_qua_bitset = (bitset_container_t *) c;
    int32_t n_runs = bitset_container_number_of_runs( c_qua_bitset);
    int32_t size_as_run_container = run_container_serialized_size_in_bytes(n_runs);
    int32_t size_as_bitset_container = bitset_container_serialized_size_in_bytes();

    if (size_as_bitset_container <= size_as_run_container) {
      // no conversion needed.
      *typecode_after = BITSET_CONTAINER_TYPE_CODE;
      return c;
    }
    // bitset to runcontainer (ported from Java  RunContainer( BitmapContainer bc, int nbrRuns))
    assert(n_runs > 0); // no empty bitmaps
    run_container_t *answer = run_container_create_given_capacity(n_runs);
        
    int long_ctr = 0;  
    uint64_t cur_word = c_qua_bitset->array[0];  
    int run_count=0;
    while (true) {
      while (cur_word == UINT64_C(0) && long_ctr < BITSET_CONTAINER_SIZE_IN_WORDS-1)
        cur_word = c_qua_bitset->array[ ++long_ctr];
     
      if (cur_word == UINT64_C(0)) {
        bitset_container_free(c);
        *typecode_after = RUN_CONTAINER_TYPE_CODE;
        return answer;
      }
     
      int local_run_start = __builtin_ctzl(cur_word);
      int run_start = local_run_start   + 64*long_ctr;
      uint64_t cur_word_with_1s = cur_word | (cur_word - 1);
     
      int run_end = 0;
      while (cur_word_with_1s == UINT64_C(-1) && long_ctr < BITSET_CONTAINER_SIZE_IN_WORDS-1)
        cur_word_with_1s = c_qua_bitset->array[++long_ctr];
     
      if (cur_word_with_1s == UINT64_C(-1)) {
        run_end = 64 + long_ctr*64; // exclusive, I guess
        add_run(answer, run_start, run_end-1); 
        bitset_container_free(c);
        *typecode_after = RUN_CONTAINER_TYPE_CODE;
        return answer;
      }
      int local_run_end = __builtin_ctzl(~cur_word_with_1s);
      run_end = local_run_end + long_ctr*64;
      add_run(answer, run_start, run_end-1); 
      run_count++;
      cur_word = cur_word_with_1s & (cur_word_with_1s + 1);
    }
    return answer;
  }
}





