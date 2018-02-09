// ----------------------------------------------------------------------------
// Copyright 2017 Nervana Systems Inc.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// ----------------------------------------------------------------------------

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <string>
#include <typeindex>
#include <unordered_map>
#include <vector>
#include <nvrtc.h>
#include <cuda.h>
#include <cuda_runtime.h>
#include <cublas_v2.h>
#include <cudnn_v7.h>

#include "ngraph/node.hpp"
#include "ngraph/ops/broadcast.hpp"
#include "ngraph/ops/concatenate.hpp"
#include "ngraph/ops/constant.hpp"
#include "ngraph/ops/convolution.hpp"
#include "ngraph/ops/dot.hpp"
#include "ngraph/ops/function_call.hpp"
#include "ngraph/ops/get_output_element.hpp"
#include "ngraph/ops/max_pool.hpp"
#include "ngraph/ops/one_hot.hpp"
#include "ngraph/ops/reduce.hpp"
#include "ngraph/ops/replace_slice.hpp"
#include "ngraph/ops/reshape.hpp"
#include "ngraph/ops/reverse.hpp"
#include "ngraph/ops/slice.hpp"
#include "ngraph/ops/sum.hpp"
#include "ngraph/runtime/gpu/gpu_emitter.hpp"
#include "ngraph/runtime/gpu/gpu_kernel_emitters.hpp"
#include "ngraph/util.hpp"

using namespace std;
using namespace ngraph;


#define NVRTC_SAFE_CALL(x) \ 
do { \ 
    nvrtcResult result = x; \ 
        if (result != NVRTC_SUCCESS) { \ 
            std::cerr << "\nerror: " #x " failed with error " \ 
                << nvrtcGetErrorString(result) << '\n'; \
                exit(1); \ 
        } \
} while(0) 

#define CUDA_SAFE_CALL(x) \ 
do { \ 
    CUresult result = x; \ 
        if (result != CUDA_SUCCESS) { \ 
            const char *msg; \ 
                cuGetErrorName(result, &msg); \ 
                std::cerr << "\nerror: " #x " failed with error " \ 
                << msg << '\n'; \ 
                exit(1); \ 
        } \ 
} while(0)


void runtime::gpu::GPU_Emitter::EmitNop(codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
}

void runtime::gpu::GPU_Emitter::EmitAbs(codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    const char *op_abs = R"(  
  extern "C" __global__  
  void cuda_op_abs(float* in, float* out, size_t n)  
  {  
    size_t tid = blockIdx.x * blockDim.x + threadIdx.x;  
    if(tid < n) 
    {
      out[tid] = fabsf(in[tid]);  
    }
  })";

        size_t numBlocks = 4; size_t numThreads = 4; 
    // Create an instance of nvrtcProgram with the code string. 

    nvrtcProgram prog; 
    NVRTC_SAFE_CALL(nvrtcCreateProgram(&prog, // prog i
                op_abs, // buffer 
                "op_abs.cu", // name 
                0, // numHeaders 
                NULL, // headers 
                NULL)); // includeNames


    const char *opts[] = {"--gpu-architecture=compute_35",
        "--relocatable-device-code=true"};
    nvrtcResult compileResult = nvrtcCompileProgram(prog, // prog 
            2, // numOptions 
            opts); // options
    // Obtain compilation log from the program. 

    size_t logSize; 

    NVRTC_SAFE_CALL(nvrtcGetProgramLogSize(prog, &logSize)); 
    char *log = new char[logSize]; 
    NVRTC_SAFE_CALL(nvrtcGetProgramLog(prog, log)); 
    std::cout << log << '\n'; 
    delete[] log; 

    if (compileResult != NVRTC_SUCCESS) {
        exit(1);
    }

    size_t ptxSize; 
    NVRTC_SAFE_CALL(nvrtcGetPTXSize(prog, &ptxSize)); 
    char *ptx = new char[ptxSize]; 
    NVRTC_SAFE_CALL(nvrtcGetPTX(prog, ptx)); // Destroy the program. 
    NVRTC_SAFE_CALL(nvrtcDestroyProgram(&prog)); // Load the generated PTX and get a handle to the parent kernel. 

    CUdevice cuDevice; 
    CUcontext context; 
    CUmodule module; 
    CUfunction cuda_op_abs_kernel; 
    CUDA_SAFE_CALL( cuInit(0)); 
    CUDA_SAFE_CALL(cuDeviceGet(&cuDevice, 0)); 
    CUDA_SAFE_CALL(cuCtxCreate(&context, 0, cuDevice)); 
    // CUDA_SAFE_CALL(cuLinkCreate(0, 0 , 0, &linkState)); 
    //CUDA_SAFE_CALL(cuLinkeAddFile(linkState, CU_JIT_INPUT_LIBRARY, ' ', 0, 0, 0));
    //CUDA_SAFE_CALL(cuLinkAddData(linkState, CU_JIT_INPUT_PTX, (void *)ptx, ptxSize, "dynamic_parallelism.ptx", 0, 0, 0));

    //size_t cubinSize;
    //void *cubin;
    //CUDA_SAFE_CALL(cuLinkComplete(linkState, &cubin, &cubinSize));
    CUDA_SAFE_CALL(cuModuleLoadDataEx(&module, ptx, 0, 0, 0));
    CUDA_SAFE_CALL(cuModuleGetFunction(&cuda_op_abs_kernel, module, "cuda_op_abs"));


    writer << "{  // " << n->get_name() << "\n";
    writer.indent++;
    writer << "int count = " << out[0].get_size() << ";\n";
    writer << "if(count == 0) return;\n";
    writer << "void *argsList[] = {(void *)" << args[0].get_name() << ", (void *)" << out[0].get_name() << ", &count};\n";
    writer << "//cuLaunchKernel(cuda_op_abs_kernel, count, 1, 1, 1, 1, 1, 0, NULL, argsList, 0);\n";
    writer.indent--;
    writer << "}\n";

    // Generate input for execution, and create output buffers. 
    //size_t nt = numBlocks * numThreads; 
    //size_t bufferSize = nt * sizeof(float); 
    //float *hOut = new float[nt]; 
    //float *hIn = new float[nt]; 
    //for(int i = 0; i< nt; i++) hIn[i] = -i;
    //
    //CUdeviceptr dOut, dIn;
    //cuMemAlloc(&dOut, bufferSize); // Execute parent kernel. 
    //cuMemAlloc(&dIn, bufferSize); // Execute parent kernel. 
    //cuMemcpyHtoD(dIn, hIn, bufferSize); 
    //
    //void *argst[] = {&dIn, &dOut, &nt};
    // CUDA_SAFE_CALL(
    // cuLaunchKernel(kernel, 
    //    numBlocks , 1, 1, // grid dim 
    //    numThreads, 1, 1, // block dim 
    //    0, NULL, // shared mem and stream 
    //   argst, 0)); // arguments 
    //CUDA_SAFE_CALL(cuCtxSynchronize()); // Retrieve and print output. 
    //cuMemcpyDtoH(hOut, dOut, bufferSize); 
    //for (size_t i = 0; i < nt; ++i) { std::cout << hOut[i] << '\n'; } // Release resources. 
    //cuMemFree(dOut); 
    //cuModuleUnload(module); 
}

void runtime::gpu::GPU_Emitter::EmitAdd(codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << "{  // " << n->get_name() << "\n";
    writer.indent++;
    writer << "int count = " << out[0].get_size() << ";\n";
    writer << "if(count == 0) return;\n";
    writer +=  R"(
float alpha1 = 1.0, alpha2 = 1.0, beta = 0;
cudnnTensorDescriptor_t descriptor;
cudnnCreateTensorDescriptor(&descriptor);
cudnnSetTensor4dDescriptor(descriptor,
                            /*format=*/CUDNN_TENSOR_NHWC,
                            /*dataType=*/CUDNN_DATA_FLOAT,
                            /*batch_size=*/1,
                            /*channels=*/1,
                            /*image_height=*/1,
                            /*image_width=*/count);

cudnnOpTensorDescriptor_t opTensorDesc;
cudnnCreateOpTensorDescriptor(&opTensorDesc);
cudnnSetOpTensorDescriptor(opTensorDesc,
                            CUDNN_OP_TENSOR_ADD,
                            CUDNN_DATA_FLOAT,
                            CUDNN_NOT_PROPAGATE_NAN);
    )";

        writer  << "cudnnOpTensor(cudnn_handle,"
        << "opTensorDesc,"
        << "&alpha1,"
        << "descriptor,"
        <<  args[0].get_name() << ","
        << "&alpha2,"
        << "descriptor,"
        << args[1].get_name() << ","
        << "&beta,"
        << "descriptor,"
        << out[0].get_name() << ");\n";
    writer.indent--;
    writer << "}\n"; 
}

void runtime::gpu::GPU_Emitter::EmitConcat(codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
}

void runtime::gpu::GPU_Emitter::EmitDot(codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << " // " << n->get_name() << "\n  return;\n";
    /*
       const Shape& arg0_shape = args[0].get_shape();
       const Shape& arg1_shape = args[1].get_shape();
       if (arg0_shape.empty() || arg1_shape.empty())
       {
       auto& first = (arg0_shape.empty() ? args[0] : args[1]);
       auto& second = (arg0_shape.empty() ? args[1] : args[0]);
       writer << "{   // " << n->get_name() << "\n";
       writer.indent++;
    // clang-format off
    writer << "cublasSdot("
    << "cublas_handle,"
    << second.get_size() << ","
    << first.get_name() << ","
    << "1,"
    << second.get_name() << ","
    << "1,"
    << out[0].get_name() << ");\n";
    // clang-format on
    writer.indent--;
    writer << "}\n";
    }

    else if ((arg0_shape.size() == 1) && (arg1_shape.size() == 1))
    {
    writer << "{   // " << n->get_name() << "\n";
    writer.indent++;
    // clang-format off
    writer << "cublasSdot("
    << "cublas_handle,"
    << arg0_shape[0] << ","
    << args[0].get_name() << ","
    << "1,"
    << args[1].get_name() << ","
    << "1,"
    << out[0].get_name() << ");\n";
    // clang-format on
    writer.indent--;
    writer << "}\n";
    }
    else if ((arg0_shape.size() == 2) && (arg1_shape.size() == 1))
    {
    writer << "{   // " << n->get_name() << "\n";
    writer.indent++;
    writer << "static const float alpha = 1.0;\n";
    writer << "static const float beta  = 1.0;\n";
    writer << "cublasSetPointerMode(cublas_handle, CUBLAS_POINTER_MODE_HOST);\n";
    ;
    // clang-format off
    writer << "cublasSgemv("
    << "cublas_handle,"
    << "CUBLAS_OP_T,"
    << arg0_shape[0] << ","
    << arg0_shape[1] << ","
    << "&alpha,"                      // Alpha
    << args[0].get_name() << ","
    << arg0_shape[1] << ","
    << args[1].get_name() << ","
    << "1,"
    << "&beta,"                      // beta
    << out[0].get_name() << ","
    << "1);\n";
    // clang-format on
    ;
    writer.indent--;
    writer << "}\n";
    }
    else if ((arg0_shape.size() == 2) && (arg1_shape.size() == 2))
    {
    // GEMM Call
    assert(arg0_shape[0] == out[0].get_shape()[0]); // m
    assert(arg1_shape[1] == out[0].get_shape()[1]); // n
    assert(arg0_shape[1] == arg1_shape[0]);         // k
    writer << "{   // " << n->get_name() << "\n";
    writer.indent++;
    writer << "static const float alpha = 1.0;\n";
    writer << "static const float beta  = 0.0;\n";
    writer << "int m = " << arg0_shape[0] << ";\n";
    writer << "int n = " << arg1_shape[1] << ";\n";
    writer << "int k = " << arg0_shape[0] << ";\n";
    writer << "cublasSetPointerMode(cublas_handle, CUBLAS_POINTER_MODE_HOST);\n";
    // clang-format off
    writer << "cublasSgemm("
        << "cublas_handle,"
        << "CUBLAS_OP_N,"
        << "CUBLAS_OP_N,"
        << "n,"
        << "m,"
        << "k,"
        << "&alpha,"                      // Alpha
        << args[1].get_name() << ","
        << "n,"
        << args[0].get_name() << ","
        << "k,"
        << "&beta,"                      // beta
        << out[0].get_name() << ","
        << "n);\n";
    // clang-format on
    writer.indent--;
    writer << "}\n";
}
else
{
    // General ND Call?
}
*/
}

void runtime::gpu::GPU_Emitter::EmitDivide(codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
}

void runtime::gpu::GPU_Emitter::EmitEqual(codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
}

void runtime::gpu::GPU_Emitter::EmitGreater(codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
}

void runtime::gpu::GPU_Emitter::EmitGreaterEq(
        codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << " // " << n->get_name() << "\n  return;\n";
}

void runtime::gpu::GPU_Emitter::EmitLess(codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << " // " << n->get_name() << "\n  return;\n";
}

void runtime::gpu::GPU_Emitter::EmitLessEq(codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << " // " << n->get_name() << "\n  return;\n";
}

void runtime::gpu::GPU_Emitter::EmitLog(codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << " // " << n->get_name() << "\n  return;\n";
}

void runtime::gpu::GPU_Emitter::EmitMaximum(codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << "{  // " << n->get_name() << "\n";
    writer.indent++;
    writer << "int count = " << out[0].get_size() << ";\n";
    writer << "if(count == 0) return;\n";
    writer +=  R"(
float alpha1 = 1.0, alpha2 = 1.0, beta = 0;
cudnnTensorDescriptor_t descriptor;
cudnnCreateTensorDescriptor(&descriptor);
cudnnSetTensor4dDescriptor(descriptor,
                            /*format=*/CUDNN_TENSOR_NHWC,
                            /*dataType=*/CUDNN_DATA_FLOAT,
                            /*batch_size=*/1,
                            /*channels=*/1,
                            /*image_height=*/1,
                            /*image_width=*/count);

cudnnOpTensorDescriptor_t opTensorDesc;
cudnnCreateOpTensorDescriptor(&opTensorDesc);
cudnnSetOpTensorDescriptor(opTensorDesc,
                            CUDNN_OP_TENSOR_MAX,
                            CUDNN_DATA_FLOAT,
                            CUDNN_NOT_PROPAGATE_NAN);
    )";

        writer  << "cudnnOpTensor(cudnn_handle,"
        << "opTensorDesc,"
        << "&alpha1,"
        << "descriptor,"
        <<  args[0].get_name() << ","
        << "&alpha2,"
        << "descriptor,"
        << args[1].get_name() << ","
        << "&beta,"
        << "descriptor,"
        << out[0].get_name() << ");\n";
    writer.indent--;
    writer << "}\n"; 
}

void runtime::gpu::GPU_Emitter::EmitMinimum(codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << "{  // " << n->get_name() << "\n";
    writer.indent++;
    writer << "int count = " << out[0].get_size() << ";\n";
    writer << "if(count == 0) return;\n";
    writer +=  R"(
float alpha1 = 1.0, alpha2 = 1.0, beta = 0;
cudnnTensorDescriptor_t descriptor;
cudnnCreateTensorDescriptor(&descriptor);
cudnnSetTensor4dDescriptor(descriptor,
                            /*format=*/CUDNN_TENSOR_NHWC,
                            /*dataType=*/CUDNN_DATA_FLOAT,
                            /*batch_size=*/1,
                            /*channels=*/1,
                            /*image_height=*/1,
                            /*image_width=*/count);

cudnnOpTensorDescriptor_t opTensorDesc;
cudnnCreateOpTensorDescriptor(&opTensorDesc);
cudnnSetOpTensorDescriptor(opTensorDesc,
                            CUDNN_OP_TENSOR_MIN,
                            CUDNN_DATA_FLOAT,
                            CUDNN_NOT_PROPAGATE_NAN);
    )";

        writer  << "cudnnOpTensor(cudnn_handle,"
        << "opTensorDesc,"
        << "&alpha1,"
        << "descriptor,"
        <<  args[0].get_name() << ","
        << "&alpha2,"
        << "descriptor,"
        << args[1].get_name() << ","
        << "&beta,"
        << "descriptor,"
        << out[0].get_name() << ");\n";
    writer.indent--;
    writer << "}\n"; 

}

void runtime::gpu::GPU_Emitter::EmitNegative(
        codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << "{  // " << n->get_name() << "\n";
    writer.indent++;
    writer << "int count = " << out[0].get_size() << ";\n";
    writer << "if(count == 0) return;\n";
    writer +=  R"(
float alpha1 = -1.0, alpha2 = 0, beta = 0;
cudnnTensorDescriptor_t descriptor;
cudnnCreateTensorDescriptor(&descriptor);
cudnnSetTensor4dDescriptor(descriptor,
                            /*format=*/CUDNN_TENSOR_NHWC,
                            /*dataType=*/CUDNN_DATA_FLOAT,
                            /*batch_size=*/1,
                            /*channels=*/1,
                            /*image_height=*/1,
                            /*image_width=*/count);

cudnnOpTensorDescriptor_t opTensorDesc;
cudnnCreateOpTensorDescriptor(&opTensorDesc);
cudnnSetOpTensorDescriptor(opTensorDesc,
                            CUDNN_OP_TENSOR_ADD,
                            CUDNN_DATA_FLOAT,
                            CUDNN_NOT_PROPAGATE_NAN);
    )";

        writer  << "cudnnOpTensor(cudnn_handle,"
        << "opTensorDesc,"
        << "&alpha1,"
        << "descriptor,"
        <<  args[0].get_name() << ","
        << "&alpha2,"
        << "descriptor,"
        << args[0].get_name() << ","
        << "&beta,"
        << "descriptor,"
        << out[0].get_name() << ");\n";
    writer.indent--;
    writer << "}\n"; 
}

void runtime::gpu::GPU_Emitter::EmitNotEqual(
        codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << " // " << n->get_name() << "\n  return;\n";
}
void runtime::gpu::GPU_Emitter::EmitSelect(codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << " // " << n->get_name() << "\n  return;\n";
}

void runtime::gpu::GPU_Emitter::EmitSubtract(
        codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << " // " << n->get_name() << "\n  return;\n";
}

void runtime::gpu::GPU_Emitter::EmitBroadcast(
        codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << " // " << n->get_name() << "\n  return;\n";
}

void runtime::gpu::GPU_Emitter::EmitConvert(codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << " // " << n->get_name() << "\n  return;\n";
}

void runtime::gpu::GPU_Emitter::EmitConstant(
        codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << " // " << n->get_name() << "\n  return;\n";
}

void runtime::gpu::GPU_Emitter::EmitReshape(codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    auto reshape = static_cast<const op::Reshape*>(n);
    writer << "{   // " << n->get_name() << "\n";
    writer.indent++;
    auto arg_shape = args[0].get_shape();
    auto arg_rank = arg_shape.size();

    auto result_shape = out[0].get_shape();
    auto& result_element_type = out[0].get_element_type();

    auto input_order = reshape->get_input_order();

    bool same_layout = is_sorted(input_order.begin(), input_order.end());

    size_t result_shape_product = 1;
    for (auto i : result_shape)
    {
        result_shape_product *= i;
    }

    // If there is no layout change or we are just going from 1^n to 1^m or a zero-size tensor,
    //  we can just copy.
    if (same_layout || result_shape_product < 2)
    {
        writer << "{   // " << n->get_name() << " 1\n";
        writer.indent++;
        writer << "runtime::gpu::cuda_memcpyDtD(" << out[0].get_name() << ", " << args[0].get_name()
            << ", " << out[0].get_size() << "," << out[0].get_element_type().size() << ");\n";
        writer.indent--;
        writer << "}\n";
    }
    // If there *is* a layout change in the 2D case, we transpose the input.
    else if (arg_rank == 2)
    {
        // clang-format off
        // TODO Assert arg0_shape[0] == arg1_shape[0]?
        writer << "{   // " << n->get_name() << "\n";
        writer.indent++;
        writer << "static const float alpha = 1.0;\n";
        writer << "static const float beta = 0.0;\n";
        writer << "cublasSetPointerMode(cublas_handle, CUBLAS_POINTER_MODE_HOST);\n";;
        writer << "cublasSgeam("
            << "cublas_handle,"
            << "CUBLAS_OP_T,"
            << "CUBLAS_OP_T,"
            << arg_shape[0] << ","
            << arg_shape[1] << ","
            << "&alpha,"                      // Alpha
            << args[0].get_name() << ","
            << arg_shape[1] << ","
            << "&beta,"                      // beta
            << args[0].get_name() << ","
            << arg_shape[1] << ","
            << out[0].get_name() << ","
            << out[0].get_shape()[1] << ");\n";
        writer.indent--;
        writer << "}\n";
        //clang-format on
    }
    // Other cases (reordering of axes for tensors with rank>2) are not handled yet.
    else
    {
        throw ngraph_error(
                "Axis permutation in reshape is not implemented yet for tensors with rank>2");
    }
    writer.indent--;
    writer << "}\n";
}

void runtime::gpu::GPU_Emitter::EmitFunctionCall(
        codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
}

void runtime::gpu::GPU_Emitter::EmitReduce(codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << " // " << n->get_name() << "\n  return;\n";
}

void runtime::gpu::GPU_Emitter::EmitSign(codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << " // " << n->get_name() << "\n  return;\n";
}

void runtime::gpu::GPU_Emitter::EmitSlice(codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << " // " << n->get_name() << "\n  return;\n";
}

void runtime::gpu::GPU_Emitter::EmitSum(codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << " // " << n->get_name() << "\n  return;\n";
}

void runtime::gpu::GPU_Emitter::EmitMultiply(
        codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << "{  // " << n->get_name() << "\n";
    writer.indent++;
    writer << "int count = " << out[0].get_size() << ";\n";
    writer << "if(count == 0) return;\n";
    writer +=  R"(
float alpha1 = 1.0, alpha2 = 1.0, beta = 0;
cudnnTensorDescriptor_t descriptor;
cudnnCreateTensorDescriptor(&descriptor);
cudnnSetTensor4dDescriptor(descriptor,
                            /*format=*/CUDNN_TENSOR_NHWC,
                            /*dataType=*/CUDNN_DATA_FLOAT,
                            /*batch_size=*/1,
                            /*channels=*/1,
                            /*image_height=*/1,
                            /*image_width=*/count);

cudnnOpTensorDescriptor_t opTensorDesc;
cudnnCreateOpTensorDescriptor(&opTensorDesc);
cudnnSetOpTensorDescriptor(opTensorDesc,
                            CUDNN_OP_TENSOR_MUL,
                            CUDNN_DATA_FLOAT,
                            CUDNN_NOT_PROPAGATE_NAN);
    )";

        writer  << "cudnnOpTensor(cudnn_handle,"
        << "opTensorDesc,"
        << "&alpha1,"
        << "descriptor,"
        <<  args[0].get_name() << ","
        << "&alpha2,"
        << "descriptor,"
        << args[1].get_name() << ","
        << "&beta,"
        << "descriptor,"
        << out[0].get_name() << ");\n";
    writer.indent--;
    writer << "}\n"; 

}

void runtime::gpu::GPU_Emitter::EmitExp(codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << " // " << n->get_name() << "\n  return;\n";
}

void runtime::gpu::GPU_Emitter::EmitSin(codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << " // " << n->get_name() << "\n  return;\n";
}

void runtime::gpu::GPU_Emitter::EmitSinh(codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << " // " << n->get_name() << "\n  return;\n";
}

void runtime::gpu::GPU_Emitter::EmitCos(codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << " // " << n->get_name() << "\n  return;\n";
}

void runtime::gpu::GPU_Emitter::EmitCosh(codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << " // " << n->get_name() << "\n  return;\n";
}

void runtime::gpu::GPU_Emitter::EmitTan(codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << " // " << n->get_name() << "\n  return;\n";
}

void runtime::gpu::GPU_Emitter::EmitTanh(codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << " // " << n->get_name() << "\n  return;\n";
}

void runtime::gpu::GPU_Emitter::EmitAsin(codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << " // " << n->get_name() << "\n  return;\n";
}

void runtime::gpu::GPU_Emitter::EmitAcos(codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << " // " << n->get_name() << "\n  return;\n";
}

void runtime::gpu::GPU_Emitter::EmitAtan(codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << " // " << n->get_name() << "\n  return;\n";
}

void runtime::gpu::GPU_Emitter::EmitPower(codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << " // " << n->get_name() << "\n  return;\n";
}

void runtime::gpu::GPU_Emitter::EmitReplaceSlice(
        codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << " // " << n->get_name() << "\n  return;\n";
}

void runtime::gpu::GPU_Emitter::EmitOneHot(codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << " // " << n->get_name() << "\n  return;\n";
}

void runtime::gpu::GPU_Emitter::EmitCeiling(codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << " // " << n->get_name() << "\n  return;\n";
}

void runtime::gpu::GPU_Emitter::EmitFloor(codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << " // " << n->get_name() << "\n  return;\n";
}

void runtime::gpu::GPU_Emitter::EmitSqrt(codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << "{  // " << n->get_name() << "\n";
    writer.indent++;
    writer << "int count = " << out[0].get_size() << ";\n";
    writer << "if(count == 0) return;\n";
    writer +=  R"(
float alpha1 = 1.0, alpha2 = 0, beta = 0;
cudnnTensorDescriptor_t descriptor;
cudnnCreateTensorDescriptor(&descriptor);
cudnnSetTensor4dDescriptor(descriptor,
                            /*format=*/CUDNN_TENSOR_NHWC,
                            /*dataType=*/CUDNN_DATA_FLOAT,
                            /*batch_size=*/1,
                            /*channels=*/1,
                            /*image_height=*/1,
                            /*image_width=*/count);

cudnnOpTensorDescriptor_t opTensorDesc;
cudnnCreateOpTensorDescriptor(&opTensorDesc);
cudnnSetOpTensorDescriptor(opTensorDesc,
                            CUDNN_OP_TENSOR_SQRT,
                            CUDNN_DATA_FLOAT,
                            CUDNN_NOT_PROPAGATE_NAN);
    )";

        writer  << "cudnnOpTensor(cudnn_handle,"
        << "opTensorDesc,"
        << "&alpha1,"
        << "descriptor,"
        <<  args[0].get_name() << ","
        << "&alpha2,"
        << "descriptor,"
        << args[0].get_name() << ","
        << "&beta,"
        << "descriptor,"
        << out[0].get_name() << ");\n";
    writer.indent--;
    writer << "}\n"; 
}

void runtime::gpu::GPU_Emitter::EmitConvolution(
        codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << " // " << n->get_name() << "\n  return;\n";
}

void runtime::gpu::GPU_Emitter::EmitNot(codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << " // " << n->get_name() << "\n  return;\n";
}

void runtime::gpu::GPU_Emitter::EmitMaxPool(codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << " // " << n->get_name() << "\n  return;\n";
}

void runtime::gpu::GPU_Emitter::EmitReverse(codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << " // " << n->get_name() << "\n  return;\n";
}

void runtime::gpu::GPU_Emitter::EmitReduceWindow(
        codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << " // " << n->get_name() << "\n  return;\n";
}

void runtime::gpu::GPU_Emitter::EmitSelectAndScatter(
        codegen::CodeWriter& writer,
        const ngraph::Node* n,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& args,
        const vector<runtime::gpu::GPU_TensorViewWrapper>& out)
{
    writer << " // " << n->get_name() << "\n  return;\n";
}
