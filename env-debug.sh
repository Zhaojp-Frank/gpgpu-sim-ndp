export CUDA_INSTALL_PATH=/usr/local/cuda-5.5
export PATH=$PATH:$CUDA_INSTALL_PATH/bin
source setup_environment debug
export NVOPENCL_LIBDIR=/usr/lib
export NVOPENCL_INCDIR=$CUDA_INSTALL_PATH/include
