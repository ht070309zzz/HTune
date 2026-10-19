// Compile only the bounded inflate implementation into the native executable.
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_MALLOC
#define MINIZ_USE_UNALIGNED_LOADS_AND_STORES 0
#define MINIZ_LITTLE_ENDIAN 1
#define MINIZ_HAS_64BIT_REGISTERS 1
#pragma warning(push, 0)
#include "third_party/miniz/miniz_tinfl.c"
#pragma warning(pop)
