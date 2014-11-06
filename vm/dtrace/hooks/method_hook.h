#define RUBINIUS_METHOD_HOOK(probe, state, mod, method, previous)
{
  if(RUBINIUS_METHOD_##probe##_ENABLED()) {
    RBX_DTRACE_CONST char* module_name =
        const_cast<RBX_DTRACE_CONST char*>(mod->debug_str(state).c_str());

    RBX_DTRACE_CONST char* code_name =
        const_cast<RBX_DTRACE_CONST char*>(method->debug_str(state).c_str());

    RBX_DTRACE_CONST char* file_name =
        const_cast<RBX_DTRACE_CONST char*>("<unknown>");

    int line = 0;

    if(previous) {
      Symbol* file = previous->file(state);

      if(!file->nil_p()) {
        file_name = const_cast<RBX_DTRACE_CONST char*>(file->debug_str(state).c_str());
      }

      line = previous->line(state);
    }

    RUBINIUS_METHOD_##probe(module_name, code_name, file_name, line);
  }
}
