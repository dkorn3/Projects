#ifndef SPO2_ALGORITHM_H_
#define SPO2_ALGORITHM_H_

#include <stdint.h>

#define FreqS       25
#define BUFFER_SIZE (FreqS * 4)
#define MA4_SIZE    4

// Defined once in spo2_algorithm.cpp
extern const uint8_t uch_spo2_table[184];
extern int32_t an_x[BUFFER_SIZE];
extern int32_t an_y[BUFFER_SIZE];

#if defined(__AVR_ATmega328P__) || defined(__AVR_ATmega168__)
void maxim_heart_rate_and_oxygen_saturation(uint16_t *pun_ir_buffer, int32_t n_ir_buffer_length, uint16_t *pun_red_buffer, int32_t *pn_spo2, int8_t *pch_spo2_valid, int32_t *pn_heart_rate, int8_t *pch_hr_valid);
#else
void maxim_heart_rate_and_oxygen_saturation(uint32_t *pun_ir_buffer, int32_t n_ir_buffer_length, uint32_t *pun_red_buffer, int32_t *pn_spo2, int8_t *pch_spo2_valid, int32_t *pn_heart_rate, int8_t *pch_hr_valid);
#endif

void maxim_find_peaks(int32_t *pn_locs, int32_t *n_npks, int32_t *pn_x, int32_t n_size, int32_t n_min_height, int32_t n_min_distance, int32_t n_max_num);
void maxim_peaks_above_min_height(int32_t *pn_locs, int32_t *n_npks, int32_t *pn_x, int32_t n_size, int32_t n_min_height);
void maxim_remove_close_peaks(int32_t *pn_locs, int32_t *pn_npks, int32_t *pn_x, int32_t n_min_distance);
void maxim_sort_ascend(int32_t *pn_x, int32_t n_size);
void maxim_sort_indices_descend(int32_t *pn_x, int32_t *pn_indx, int32_t n_size);

#endif /* SPO2_ALGORITHM_H_ */