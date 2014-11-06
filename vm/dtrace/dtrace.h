#ifndef RUBINIUS_DTRACE_H
#define RUBINIUS_DTRACE_H

#ifdef HAVE_DTRACE

#include "dtrace/probes.h"
#include "dtrace/hooks.h"

#else

#include "dtrace/probes_dummy.h"

#endif

#define RUBINIUS_METHOD_ENTRY_HOOK(state, module, method, previous) \
    RUBINIUS_METHOD_HOOK(ENTRY, state, module, method, previous)

#define RUBINIUS_METHOD_RETURN_HOOK(state, module, method, previous) \
    RUBINIUS_METHOD_HOOK(RETURN, state, module, method, previous)

#define RUBINIUS_METHOD_NATIVE_ENTRY_HOOK(state, module, method, previous) \
    RUBINIUS_METHOD_HOOK(NATIVE_ENTRY, state, module, method, previous)

#define RUBINIUS_METHOD_NATIVE_RETURN_HOOK(state, module, method, previous) \
    RUBINIUS_METHOD_HOOK(NATIVE_RETURN, state, module, method, previous)

#define RUBINIUS_METHOD_FFI_ENTRY_HOOK(state, module, method, previous) \
    RUBINIUS_METHOD_HOOK(FFI_ENTRY, state, module, method, previous)

#define RUBINIUS_METHOD_FFI_RETURN_HOOK(state, module, method, previous) \
    RUBINIUS_METHOD_HOOK(FFI_RETURN, state, module, method, previous)

#define RUBINIUS_METHOD_PRIMITIVE_ENTRY_HOOK(state, module, method, previous) \
    RUBINIUS_METHOD_HOOK(PRIMITIVE_ENTRY, state, module, method, previous)

#define RUBINIUS_METHOD_PRIMITIVE_RETURN_HOOK(state, module, method, previous) \
    RUBINIUS_METHOD_HOOK(PRIMITIVE_RETURN, state, module, method, previous)

#endif
