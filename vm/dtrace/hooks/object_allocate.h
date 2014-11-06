#define RUBINIUS_OBJECT_ALLOCATE_HOOK(state, obj, frame)
{
  if(RUBINIUS_OBJECT_ALLOCATE_ENABLED()) {
    Class* mod = obj->direct_class(state);

    RBX_DTRACE_CONST char* module_name =
      const_cast<RBX_DTRACE_CONST char*>(mod->debug_str(state).c_str());

    RBX_DTRACE_CONST char* file_name =
      const_cast<RBX_DTRACE_CONST char *>("<unknown>");

    int line = 0;

    if(frame) {
      Symbol* file = frame->file(state);

      if(!file->nil_p()) {
        file_name = const_cast<RBX_DTRACE_CONST char*>(file->debug_str(state).c_str());
      }

      line = frame->line(state);
    }

    RUBINIUS_OBJECT_ALLOCATE(module_name, file_name, line);
  }
}
