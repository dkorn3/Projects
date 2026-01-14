    //Written by Dominik Kornak
    #include <stdio.h>
    #include <stdint.h>
    #include <string.h>
    #include "esp_timer.h"
    #include "esp_log.h"
    #include "esp_attr.h"
    #include "esp_system.h"
    #include "spi_flash_mmap.h" 
#include "esp_flash.h"     
#include "esp_heap_caps.h"
#include <inttypes.h>
    extern const int8_t model_weights_bin_start[] asm("_binary_model_weights_new_bin_start");
    extern const int8_t model_weights_bin_end[]   asm("_binary_model_weights_new_bin_end");

    extern const int8_t dog_image_bin_start[] asm("_binary_dog_image_bin_start");
    extern const int8_t dog_image_bin_end[]   asm("_binary_dog_image_bin_end");
    extern const int8_t car_image_bin_start[] asm("_binary_car_image_bin_start");
    extern const int8_t car_image_bin_end[]   asm("_binary_car_image_bin_end");
    extern const int8_t truck_image_bin_start[] asm("_binary_truck_image_bin_start");
    extern const int8_t truck_image_bin_end[]   asm("_binary_truck_image_bin_end");
    extern const int8_t bird_image_bin_start[] asm("_binary_bird_image_bin_start");
    extern const int8_t bird_image_bin_end[]   asm("_binary_bird_image_bin_end");
    extern const int8_t airplane_image_bin_start[] asm("_binary_airplane_image_bin_start");
    extern const int8_t airplane_image_bin_end[]   asm("_binary_airplane_image_bin_end");
        extern const int8_t cat_image_bin_start[] asm("_binary_cat_image_bin_start");
    extern const int8_t cat_image_bin_end[]   asm("_binary_cat_image_bin_end");
        extern const int8_t frog_image_bin_start[] asm("_binary_frog_image_bin_start");
    extern const int8_t frog_image_bin_end[]   asm("_binary_frog_image_bin_end");
        extern const int8_t deer_image_bin_start[] asm("_binary_deer_image_bin_start");
    extern const int8_t deer_image_bin_end[]   asm("_binary_deer_image_bin_end");
        extern const int8_t horse_image_bin_start[] asm("_binary_horse_image_bin_start");
    extern const int8_t horse_image_bin_end[]   asm("_binary_horse_image_bin_end");
        extern const int8_t ship_image_bin_start[] asm("_binary_ship_image_bin_start");
    extern const int8_t ship_image_bin_end[]   asm("_binary_ship_image_bin_end");
  

    const float conv1_bias_scale = 0.004923f;
    const float conv1_weight_scale = 0.004610f;
    const float conv2_bias_scale = 0.003761f;
    const float conv2_weight_scale = 0.009670f;
    const float fc1_bias_scale = 0.003406f;
    const float fc1_weight_scale = 0.007530f;
    const float fc2_bias_scale = 0.001714f;
    const float fc2_weight_scale = 0.007421f;

    #define CONV1_BIAS_QUANTIZED_SIZE 25
    #define CONV1_WEIGHT_QUANTIZED_SIZE 675
    #define CONV2_BIAS_QUANTIZED_SIZE 51
    #define CONV2_WEIGHT_QUANTIZED_SIZE 11475
    #define FC1_BIAS_QUANTIZED_SIZE 76
    #define FC1_WEIGHT_QUANTIZED_SIZE 15504
    #define FC2_BIAS_QUANTIZED_SIZE 10
    #define FC2_WEIGHT_QUANTIZED_SIZE 760

    #define MAX_BUF 128  
    #define IN_CH 3
    #define IN_H 32
    #define IN_W 32
    #define CONV1_OUT 25
    #define CONV2_OUT 51
    #define GAP_H 2
    #define GAP_W 2
    #define FC1_OUT 76
    #define FC1_IN (CONV2_OUT*GAP_H*GAP_W) 
    #define FC2_OUT 10
    #define FC2_IN 76 
    #define IMAGE_SIZE (IN_CH*IN_H*IN_W)
    static float conv1_rowbuf[CONV1_OUT*32*32];   
    static float conv2_rowbuf[CONV2_OUT*16*16];    
    static float gap_buf[CONV2_OUT*GAP_H*GAP_W];  
    static float fc_tmpbuf[FC1_OUT];            
    static float fc2_out[FC2_OUT];
    int8_t *model_weights_ram = NULL;
     size_t model_weights_size = 0;
   int8_t *model_image_ram = NULL;
     size_t model_image_size = 0;

// Convolution
void IRAM_ATTR conv(const float *  input, float *  output,const int8_t *  w_start, const int8_t *  b_start,float w_scale, float b_scale,size_t in_ch, size_t out_ch,size_t H, size_t W,size_t kH, size_t kW, size_t stride)
{
    const size_t HW = H * W;
    const size_t kHW = kH * kW;
    const size_t per_oc = in_ch * kHW;
    const int outH = H; 
    const int outW = W;
    const int outHW = outH * outW;
    const int pad_h = kH / 2;
    const int pad_w = kW / 2;

    float bias_fp[64]; 
    for (size_t oc = 0; oc < out_ch; oc++) {
        bias_fp[oc] = b_start[oc] * b_scale;
    }
    for (size_t oc = 0; oc < out_ch; oc++) {
        float *out_ptr = output + oc * outHW;
        const int8_t *w_oc = w_start + oc * per_oc;

        for (int i = 0; i < outHW; i++) {
            out_ptr[i] = bias_fp[oc];
        }
        for (size_t ic = 0; ic < in_ch; ic++) {
            const float *in_ic = input + ic * HW;
            const int8_t *w_ic = w_oc + ic * kHW;
            float w_fp[9]; 
            for (size_t k = 0; k < kHW; k++) {
                w_fp[k] = w_ic[k] * w_scale;
            }
            
            if (kH == 3 && kW == 3) {
                for (int oy = 1; oy < outH - 1; oy++) {
                    int iy = oy - pad_h;  
                    const float *in_row0 = in_ic + iy * W;
                    const float *in_row1 = in_row0 + W;
                    const float *in_row2 = in_row1 + W;
                    
                    for (int ox = 1; ox < outW - 1; ox++) {
                        int ix = ox - pad_w;  
                        
                   
                        float sum =  in_row0[ix]   * w_fp[0] + in_row0[ix+1] * w_fp[1] + in_row0[ix+2] * w_fp[2] +
                            in_row1[ix]   * w_fp[3] + in_row1[ix+1] * w_fp[4] + in_row1[ix+2] * w_fp[5] +
                            in_row2[ix]   * w_fp[6] + in_row2[ix+1] * w_fp[7] + in_row2[ix+2] * w_fp[8];
                        
                        out_ptr[oy * outW + ox] += sum;
                    }
                }
                for (int oy = 0; oy < outH; oy++) {
                    if (oy > 0 && oy < outH - 1)
                         continue;
                    
                    int iy_base = oy * stride - pad_h;
                    
                    for (int ox = 0; ox < outW; ox++) {
                        int ix_base = ox * stride - pad_w;
                        float sum = 0.0f;
                        
                        for (int ky = 0; ky < 3; ky++) {
                            int iy = iy_base + ky;
                            if ((unsigned)iy >= (unsigned)H)
                                 continue; 
                            
                            const float *in_row = in_ic + iy * W;
                            
                            int ix0 = ix_base;
                            int ix1 = ix_base + 1;
                            int ix2 = ix_base + 2;
                            
                            if ((unsigned)ix0 < (unsigned)W) 
                             sum += in_row[ix0] * w_fp[ky*3];
                            if ((unsigned)ix1 < (unsigned)W) 
                             sum += in_row[ix1] * w_fp[ky*3+1];
                            if ((unsigned)ix2 < (unsigned)W) 
                                sum += in_row[ix2] * w_fp[ky*3+2];
                        }
                        out_ptr[oy * outW + ox] += sum;
                    }
                }
                for (int oy = 1; oy < outH - 1; oy++) {
                    int iy_base = oy * stride - pad_h;
                    
                    for (int ox = 0; ox < outW; ox++) {
                        if (ox > 0 && ox < outW - 1)
                         continue;
                        
                        int ix_base = ox * stride - pad_w;
                        float sum = 0.0f;
                        
                        for (int ky = 0; ky < 3; ky++) {
                            int iy = iy_base + ky;
                            const float *in_row = in_ic + iy * W;
                            
                            int ix0 = ix_base;
                            int ix1 = ix_base + 1;
                            int ix2 = ix_base + 2;
                            
                            if ((unsigned)ix0 < (unsigned)W) sum += in_row[ix0] * w_fp[ky*3];
                            if ((unsigned)ix1 < (unsigned)W) sum += in_row[ix1] * w_fp[ky*3+1];
                            if ((unsigned)ix2 < (unsigned)W) sum += in_row[ix2] * w_fp[ky*3+2];
                        }
                        out_ptr[oy * outW + ox] += sum;
                    }
                }
            } else {
                int out_idx = 0;
                for (int oy = 0; oy < outH; oy++) {
                    int iy_base = oy * stride - pad_h;
                    
                    for (int ox = 0; ox < outW; ox++, out_idx++) {
                        int ix_base = ox * stride - pad_w;
                        float sum = 0.0f;
                        
                        for (size_t ky = 0; ky < kH; ky++) {
                            int iy = iy_base + ky;
                            if ((unsigned)iy >= (unsigned)H)
                             continue;

                            const float *in_row = in_ic + iy * W;
                            const float *w_row = w_fp + ky * kW;

                            for (size_t kx = 0; kx < kW; kx++) {
                                int ix = ix_base + kx;
                                if ((unsigned)ix >= (unsigned)W) 
                                continue;
                                sum += in_row[ix] * w_row[kx];
                            }
                        }
                        out_ptr[out_idx] += sum;
                    }
                }
            }
        }
    }
}
//Adaptive Average Pooling
void IRAM_ATTR adaptive_avg_pool2x2(const float*  input, float*  output,size_t in_ch, size_t H, size_t W)
    {
    const size_t strideH = H / 2;
    const size_t strideW = W / 2;
    const float inv_area = 1.0f / (strideH * strideW);

    for (size_t c = 0; c < in_ch; c++)
    {
        const float *in_c = input + c * H * W;
        float *out_c = output + c * 4;
        float sum00 = 0.0f;
        float sum01 = 0.0f;
        float sum10 = 0.0f;
        float sum11 = 0.0f;

        for (size_t y = 0; y < strideH; y++)
        {
            const float *row0 = in_c + y * W;
            const float *row1 = in_c + (y + strideH) * W;

            for (size_t x = 0; x < strideW; x++)
            {
                sum00 += row0[x];
                sum01 += row0[x + strideW];
                sum10 += row1[x];
                sum11 += row1[x + strideW];
            }
            }
        out_c[0] = sum00 * inv_area;
        out_c[1] = sum01 * inv_area;
        out_c[2] = sum10 * inv_area;
        out_c[3] = sum11 * inv_area;
    }
}

// max pooling 
void IRAM_ATTR maxpool2x2(const float* input, float* output,size_t in_ch, size_t H, size_t W)
{
    size_t outH = H / 2;
    size_t outW = W / 2;
    size_t HW = H * W;
    size_t outHW = outH * outW;

    for (size_t c = 0; c < in_ch; c++) {
        const float* in_c  = input  + c * HW;
        float*       out_c = output + c * outHW;

        for (size_t oy = 0; oy < outH; oy++) {
            size_t iy = oy * 2;

            for (size_t ox = 0; ox < outW; ox++) {
                size_t ix = ox * 2;
                 const float *row0 = in_c + iy * W;
                 const float *row1 = row0 + W;
                float a = row0[ix] > 0 ? row0[ix] : 0;
                float b = row0[ix+1] > 0 ? row0[ix+1] : 0;
                float c = row1[ix] > 0 ? row1[ix] : 0;
                float d = row1[ix+1] > 0 ? row1[ix+1] : 0;

                float m1 = (a > b ? a : b);
                float m2 = (c > d ? c : d);
                out_c[oy * outW + ox] = (m1 > m2 ? m1 : m2);
            }
        }
    }
}
// FC layer
void IRAM_ATTR fc(const float *  input, float *  output, size_t in_size, size_t out_size,const int8_t *  w_start, const int8_t *  b_start, float w_scale, float b_scale)
{
    for (size_t o = 0; o < out_size; o++) {
        const int8_t *w = w_start + o * in_size;
        float sum = b_start[o] * b_scale;

        size_t i = 0;
        for (; i + 7 < in_size; i += 8) {
            sum += input[i]   * (w[i]   * w_scale);
            sum += input[i+1] * (w[i+1] * w_scale);
            sum += input[i+2] * (w[i+2] * w_scale);
            sum += input[i+3] * (w[i+3] * w_scale);
            sum += input[i+4] * (w[i+4] * w_scale);
            sum += input[i+5] * (w[i+5] * w_scale);
            sum += input[i+6] * (w[i+6] * w_scale);
            sum += input[i+7] * (w[i+7] * w_scale);
        }
        for (; i < in_size; i++) {
            sum += input[i] * (w[i] * w_scale);
        }

        output[o] = sum;
    }
}

//Relu
    void IRAM_ATTR relu(float *x, size_t size) {
    size_t i = 0;
    for (; i + 7 < size; i += 8) {
        x[i]   = x[i]   > 0 ? x[i]   : 0;
        x[i+1] = x[i+1] > 0 ? x[i+1] : 0;
        x[i+2] = x[i+2] > 0 ? x[i+2] : 0;
        x[i+3] = x[i+3] > 0 ? x[i+3] : 0;
        x[i+4] = x[i+4] > 0 ? x[i+4] : 0;
        x[i+5] = x[i+5] > 0 ? x[i+5] : 0;
        x[i+6] = x[i+6] > 0 ? x[i+6] : 0;
        x[i+7] = x[i+7] > 0 ? x[i+7] : 0;
    }

    for (; i < size; i++) {
        x[i] = x[i] > 0 ? x[i] : 0;
    }

}

   void IRAM_ATTR run_inference()
{
    const int8_t* bin = model_weights_ram; 
    size_t off = 0;
    const int8_t* image_data = model_image_ram;
    static float image[IN_CH*IN_H*IN_W];
    static const float mean[3] = {0.4914f, 0.4822f, 0.4465f};
    static const float stdv[3]  = {0.2023f, 0.1994f, 0.2010f};

    for (int i = 0; i < IMAGE_SIZE; i++) {
        int c = i / (IN_H * IN_W);
        float x = (image_data[i] + 128) / 255.0f;
        x = (x - mean[c]) / stdv[c];
        image[i] = x;
    }


    // Conv1
    conv(image, conv1_rowbuf,bin + off + CONV1_BIAS_QUANTIZED_SIZE,bin + off,conv1_weight_scale, conv1_bias_scale,IN_CH, CONV1_OUT, 32, 32, 3, 3, 1);

        off += CONV1_BIAS_QUANTIZED_SIZE + CONV1_WEIGHT_QUANTIZED_SIZE;
    //maxpool
    maxpool2x2(conv1_rowbuf, conv1_rowbuf, CONV1_OUT, 32, 32);

    // Conv2
    conv(conv1_rowbuf, conv2_rowbuf,bin + off + CONV2_BIAS_QUANTIZED_SIZE,bin + off,conv2_weight_scale, conv2_bias_scale, CONV1_OUT, CONV2_OUT, 16, 16, 3, 3, 1);
    off += CONV2_BIAS_QUANTIZED_SIZE + CONV2_WEIGHT_QUANTIZED_SIZE;
    //maxpool
    maxpool2x2(conv2_rowbuf, conv2_rowbuf, CONV2_OUT, 16, 16);

    // Adaptive average pooling
    adaptive_avg_pool2x2(conv2_rowbuf, gap_buf, CONV2_OUT, 8, 8);

      // FC1
  const int8_t *fc1_bias = bin + off;
const int8_t *fc1_weight = bin + off + FC1_BIAS_QUANTIZED_SIZE;
fc(gap_buf, fc_tmpbuf, FC1_IN, FC1_OUT,fc1_weight, fc1_bias,fc1_weight_scale, fc1_bias_scale);
//relu
relu(fc_tmpbuf, FC1_OUT);
off += FC1_BIAS_QUANTIZED_SIZE + FC1_WEIGHT_QUANTIZED_SIZE;

// FC2
     const int8_t *fc2_bias = bin + off;
const int8_t *fc2_weight = bin + off + FC2_BIAS_QUANTIZED_SIZE;
fc(fc_tmpbuf, fc2_out, FC2_IN, FC2_OUT,fc2_weight, fc2_bias,fc2_weight_scale, fc2_bias_scale);
off += FC2_BIAS_QUANTIZED_SIZE + FC2_WEIGHT_QUANTIZED_SIZE;

    printf("OUTPUT:   ");
    for (int i = 0; i < FC2_OUT; i++) printf("%f ", fc2_out[i]);
    printf("\n");

   
}
    // Main
    void IRAM_ATTR app_main(void)
    {
 
    model_weights_size = model_weights_bin_end - model_weights_bin_start;
    model_weights_ram = malloc(model_weights_size);
    memcpy(model_weights_ram, model_weights_bin_start, model_weights_size);
    model_image_size = dog_image_bin_end - dog_image_bin_start;
    model_image_ram = malloc(model_image_size);
    memcpy(model_image_ram, dog_image_bin_start, model_image_size);
        int64_t start_us = esp_timer_get_time();
        run_inference();
        int64_t end_us = esp_timer_get_time();
        printf("Inference time: %.2f ms\n", (end_us - start_us)/1000.0);

    const char* class_names[10] = {
        "airplane", "automobile", "bird", "cat", "deer",
        "dog", "frog", "horse", "ship", "truck"
    };
    typedef struct {
        int idx;
        float score;
    } Prediction;
    Prediction top3[3] = {{0, fc2_out[0]}, {1, fc2_out[1]}, {2, fc2_out[2]}};
    for (int i = 0; i < 3; i++) {
        for (int j = i + 1; j < 3; j++) {
            if (top3[j].score > top3[i].score) {
                Prediction temp = top3[i];
                top3[i] = top3[j];
                top3[j] = temp;
            }
        }
    }
    
    for (int i = 3; i < FC2_OUT; i++) {
        if (fc2_out[i] > top3[2].score) {
            top3[2].idx = i;
            top3[2].score = fc2_out[i];
            if (top3[2].score > top3[1].score) {
                Prediction temp = top3[2];
                top3[2] = top3[1];
                top3[1] = temp;
                
                if (top3[1].score > top3[0].score) {
                    temp = top3[1];
                    top3[1] = top3[0];
                    top3[0] = temp;
                }
            }
        }
    }

        printf("\nTop-3 Predictions\n");
    printf("1. %s (%.4f)\n", class_names[top3[0].idx], top3[0].score);
    printf("2. %s (%.4f)\n", class_names[top3[1].idx], top3[1].score);
    printf("3. %s (%.4f)\n", class_names[top3[2].idx], top3[2].score);
    printf("\nPredicted class: %d (%s)\n", top3[0].idx, class_names[top3[0].idx]);
    }
