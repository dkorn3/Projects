#ifndef CNN_H
#define CNN_H

#include <stdint.h>

// =====================================================================
// Architecture constants (must match CNN.c and model exactly)
// =====================================================================
#define IN_CH      1
#define IN_H       64
#define IN_W       64
#define CONV1_OUT  16
#define CONV2_OUT  32
#define CONV3_OUT  64   // Block 3 has 64 filters
#define FC_OUT     3

// =====================================================================
// Activation buffers (allocated in cnn_allocate_buffers)
// =====================================================================
extern float   *conv1_buf;
extern float   *conv2_buf;
extern float   *conv3_buf;
extern float   *gap_buf;
extern float   *fc_out;
extern float   *image_fp;
extern uint8_t *im;

// =====================================================================
// Prediction result
// =====================================================================
typedef struct {
    char  class_name[16];  
    float confidence;     
    float prob[FC_OUT];   
} cnn_prediction_t;

// =====================================================================
// Public API
// =====================================================================
void             cnn_allocate_buffers(void);
void             run_inference(void);
cnn_prediction_t cnn_get_last_prediction(void);
#endif // CNN_H