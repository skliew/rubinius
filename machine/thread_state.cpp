#include "call_frame.hpp"
#include "environment.hpp"
#include "machine.hpp"
#include "helpers.hpp"
#include "machine_code.hpp"
#include "memory.hpp"
#include "park.hpp"
#include "signal.hpp"
#include "thread_state.hpp"

#include "class/compiled_code.hpp"
#include "class/exception.hpp"
#include "class/location.hpp"
#include "class/array.hpp"
#include "class/fiber.hpp"
#include "class/unwind_state.hpp"

#include "diagnostics/machine.hpp"

// Reset macros since we're inside state
#undef G
#undef GO
#define G(whatever) globals().whatever.get()
#define GO(whatever) globals().whatever

namespace rubinius {
  utilities::thread::ThreadData<ThreadState*> _current_thread;

  ThreadState::ThreadState(uint32_t id, Machine* m, const char* name)
    : kind_(eThread)
    , metrics_(new diagnostics::MachineMetrics())
    , os_thread_(0)
    , id_(id)
    , _machine_(m)
    , call_frame_(nullptr)
    , park_(new Park)
    , thca_(new memory::OpenTHCA)
    , stack_start_(0)
    , stack_barrier_start_(0)
    , stack_barrier_end_(0)
    , stack_size_(0)
    , stack_cushion_(m->configuration()->machine_stack_cushion.value)
    , stack_probe_(0)
    , interrupt_with_signal_(false)
    , interrupt_by_kill_(false)
    , check_local_interrupts_(false)
    , thread_step_(false)
    , fiber_wait_mutex_()
    , fiber_wait_condition_()
    , fiber_transition_flag_(eSuspending)
    , interrupt_lock_()
    , method_missing_reason_(eNone)
    , constant_missing_reason_(vFound)
    , zombie_(false)
    , main_thread_(false)
    , thread_phase_(ThreadNexus::eUnmanaged)
    , sample_interval_(0)
    , sample_counter_(0)
    , checkpoints_(0)
    , stops_(0)
    , waiting_channel_(nil<Channel>())
    , interrupted_exception_(nil<Exception>())
    , thread_(nil<Thread>())
    , fiber_(nil<Fiber>())
    , waiting_object_(cNil)
    , start_time_(0)
    , native_method_environment(nullptr)
    , custom_wakeup_(nullptr)
    , custom_wakeup_data_(nullptr)
    , unwind_state_(nullptr)
  {
    if(name) {
      name_ = std::string(name);
    } else {
      std::ostringstream thread_name;
      thread_name << "ruby." << id_;
      name_ = thread_name.str();
    }

    set_sample_interval();
  }

  ThreadState::~ThreadState() {
    logger::info("%s: checkpoints: %ld, stops: %ld",
        name().c_str(), checkpoints_, stops_);

    if(park_) {
      delete park_;
      park_ = nullptr;
    }

    if(thca_) {
      delete thca_;
      thca_ = nullptr;
    }
  }

  void ThreadState::raise_exception(Exception* exc) {
    unwind_state()->raise_exception(exc);
  }

  void ThreadState::raise_stack_error() {
    Class* stack_error = globals().stack_error.get();
    Exception* exc = memory()->new_object<Exception>(this, stack_error);
    exc->locations(this, Location::from_call_stack(this));
    unwind_state()->raise_exception(exc);
  }

  Object* ThreadState::park(STATE) {
    return park_->park(this);
  }

  Object* ThreadState::park_timed(STATE, struct timespec* ts) {
    return park_->park_timed(this, ts);
  }

  const uint32_t ThreadState::hash_seed() {
    return machine()->machine_state()->hash_seed();
  }

  MachineState* const ThreadState::machine_state() {
    return machine()->machine_state();
  }

  Configuration* const ThreadState::configuration() {
    return machine()->configuration();
  }

  Environment* const ThreadState::environment() {
    return machine()->environment();
  }

  ThreadNexus* const ThreadState::thread_nexus() {
    return machine()->thread_nexus();
  }

  Diagnostics* const ThreadState::diagnostics() {
    return machine()->diagnostics();
  }

  memory::Collector* const ThreadState::collector() {
    return machine()->collector();
  }

  SignalThread* const ThreadState::signals() {
    return machine()->signals();
  }

  Memory* const ThreadState::memory() {
    return machine()->memory();
  }

  C_API* const ThreadState::c_api() {
    return machine()->c_api();
  }

  Profiler* const ThreadState::profiler() {
    return machine()->profiler();
  }

  Console* const ThreadState::console() {
    return machine()->console();
  }

  Globals& ThreadState::globals() {
    return memory()->globals;
  }

  Symbol* const ThreadState::symbol(const char* str) {
    return memory()->symbols.lookup(this, str, strlen(str));
  }

  Symbol* const ThreadState::symbol(const char* str, size_t len) {
    return memory()->symbols.lookup(this, str, len);
  }

  Symbol* const ThreadState::symbol(std::string str) {
    return memory()->symbols.lookup(this, str);
  }

  Symbol* const ThreadState::symbol(String* str) {
    return memory()->symbols.lookup(this, str);
  }

  // -*-*-*-
  void ThreadState::set_name(STATE, const char* name) {
    if(pthread_self() == os_thread_) {
      utilities::thread::Thread::set_os_name(name);
    }
    name_.assign(name);
  }

  ThreadState* ThreadState::current() {
    return _current_thread.get();
  }

  void ThreadState::set_current_thread() {
    utilities::thread::Thread::set_os_name(name().c_str());
    os_thread_ = pthread_self();
    _current_thread.set(this);
  }

  void ThreadState::discard(STATE, ThreadState* vm) {
    state->metrics()->threads_destroyed++;

    delete vm;
  }

  void ThreadState::clear_interrupted_exception() {
    interrupted_exception_ = nil<Exception>();
  }

  void ThreadState::set_thread(Thread* thread) {
    thread_ = thread;
  }

  void ThreadState::set_fiber(Fiber* fiber) {
    fiber_ = fiber;
  }

  void ThreadState::set_start_time() {
    start_time_ = get_current_time();
  }

  double ThreadState::run_time() {
    return timer::time_elapsed_seconds(start_time_);
  }

  void ThreadState::set_previous_frame(CallFrame* frame) {
    frame->previous = call_frame_;
  }

  void ThreadState::raise_stack_error(STATE) {
    state->raise_stack_error();
  }

  void ThreadState::validate_stack_size(STATE, size_t size) {
    if(stack_cushion_ > size) {
      Exception::raise_runtime_error(state, "requested stack size is invalid");
    }
  }

  UnwindState* ThreadState::unwind_state() {
    if(!unwind_state_) {
      unwind_state_ = UnwindState::create(this);
    }

    return unwind_state_;
  }

  bool ThreadState::check_thread_raise_or_kill(STATE) {
    Exception* exc = interrupted_exception();

    if(!exc->nil_p()) {
      clear_interrupted_exception();

      // Only write the locations if there are none.
      if(exc->locations()->nil_p() || exc->locations()->size() == 0) {
        exc->locations(state, Location::from_call_stack(state));
      }

      unwind_state()->raise_exception(exc);

      return true;
    }

    if(interrupt_by_kill()) {
      if(state->thread()->current_fiber()->root_p()) {
        clear_interrupt_by_kill();
      } else {
        set_check_local_interrupts();
      }

      unwind_state()->raise_thread_kill();

      return true;
    }

    // If the current thread is trying to step, debugger wise, then assist!
    if(thread_step()) {
      clear_thread_step();
      if(!Helpers::yield_debugger(state, cNil)) return true;
    }

    return false;
  }

  CallFrame* ThreadState::get_call_frame(ssize_t up) {
    CallFrame* frame = call_frame_;

    while(frame && up-- > 0) {
      frame = frame->previous;
    }

    return frame;
  }

  CallFrame* ThreadState::get_ruby_frame(ssize_t up) {
    CallFrame* frame = call_frame_;

    while(frame && up-- > 0) {
      frame = frame->previous;
    }

    while(frame) {
      if(!frame->native_method_p()) return frame;
      frame = frame->previous;
    }

    return NULL;
  }

  CallFrame* ThreadState::get_noncore_frame(STATE) {
    for(CallFrame* frame = call_frame_; frame; frame = frame->previous) {
      if(frame->native_method_p()) continue;

      CompiledCode* code = frame->compiled_code;
      if(code && !code->nil_p()) {
        if(!code->core_method(state)) return frame;
      }
    }

    return NULL;
  }

  CallFrame* ThreadState::get_filtered_frame(STATE, const std::regex& filter) {
    for(CallFrame* frame = call_frame_; frame; frame = frame->previous) {
      if(frame->native_method_p()) continue;

      CompiledCode* code = frame->compiled_code;
      if(code && !code->nil_p() && !code->file()->nil_p()) {
        if(!std::regex_match(code->file()->cpp_str(state), filter)) {
          return frame;
        }
      }
    }

    return NULL;
  }

  CallFrame* ThreadState::get_variables_frame(ssize_t up) {
    CallFrame* frame = call_frame_;

    while(frame && up-- > 0) {
      frame = frame->previous;
    }

    while(frame) {
      if(!frame->is_inline_block()
          && !frame->native_method_p()
          && frame->scope)
      {
        return frame;
      }

      frame = frame->previous;
    }

    return NULL;
  }

  CallFrame* ThreadState::get_scope_frame(ssize_t up) {
    CallFrame* frame = call_frame_;

    while(frame && up-- > 0) {
      frame = frame->previous;
    }

    while(frame) {
      if(frame->scope) return frame;
      frame = frame->previous;
    }

    return NULL;
  }

  bool ThreadState::scope_valid_p(VariableScope* scope) {
    CallFrame* frame = call_frame_;

    while(frame) {
      if(frame->scope && frame->scope->on_heap() == scope) {
        return true;
      }

      frame = frame->previous;
    }

    return false;
  }

  void ThreadState::sample(STATE) {
    timer::StopWatch<timer::nanoseconds> timer(metrics()->sample_ns);

    metrics()->samples++;

    CallFrame* frame = state->call_frame();

    while(frame) {
      // TODO: add counters to native method frames
      if(frame->compiled_code) {
        frame->compiled_code->machine_code()->sample_count++;
      }

      frame = frame->previous;
    }
  }

  static void suspend_thread() {
    static int i = 0;
    static int delay[] = {
      45, 17, 38, 31, 10, 40, 13, 37, 16, 37, 1, 20, 23, 43, 38, 4, 2, 26, 25, 5
    };
    static int modulo = sizeof(delay) / sizeof(int);
    static struct timespec ts = {0, 0};

    ts.tv_nsec = delay[i++ % modulo];

    nanosleep(&ts, NULL);
  }

  void ThreadState::set_zombie(STATE) {
    state->machine()->thread_nexus()->delete_vm(this);
    set_zombie();
  }

  void ThreadState::set_zombie() {
    set_thread(nil<Thread>());
    set_fiber(nil<Fiber>());
    zombie_ = true;
  }

  void type_assert(STATE, Object* obj, object_type type, const char* reason) {
    if((obj->reference_p() && obj->type_id() != type)
        || (type == FixnumType && !obj->fixnum_p())) {
      std::ostringstream msg;
      msg << reason << ": " << obj->to_string(state, true);
      Exception::raise_type_error(state, type, obj, msg.str().c_str());
    }
  }

  void ThreadState::after_fork_child() {
    interrupt_lock_.unlock();
    set_main_thread();

    set_start_time();

    // TODO: Remove need for root_vm.
    environment()->set_root_vm(this);
  }

  Object* ThreadState::path2class(STATE, const char* path) {
    Module* mod = state->memory()->globals.object.get();

    char* copy = strdup(path);
    char* cur = copy;

    for(;;) {
      char* pos = strstr(cur, "::");
      if(pos) *pos = 0;

      Object* obj = mod->get_const(state, state->symbol(cur));

      if(pos) {
        if(Module* m = try_as<Module>(obj)) {
          mod = m;
        } else {
          free(copy);
          return cNil;
        }
      } else {
        free(copy);
        return obj;
      }

      cur = pos + 2;
    }
  }

  bool ThreadState::wakeup(STATE) {
    std::lock_guard<locks::spinlock_mutex> guard(interrupt_lock_);

    set_check_local_interrupts();
    Object* wait = waiting_object_;

    if(park_->parked_p()) {
      park_->unpark();
      return true;
    } else if(interrupt_with_signal_) {
#ifdef RBX_WINDOWS
      // TODO: wake up the thread
#else
      pthread_kill(os_thread_, SIGVTALRM);
#endif
      interrupt_lock_.unlock();
      return true;
    } else if(!wait->nil_p()) {
      interrupt_lock_.unlock();
      return true;
    } else {
      Channel* chan = waiting_channel_;

      if(!chan->nil_p()) {
        interrupt_lock_.unlock();
        chan->send(state, cNil);
        return true;
      } else if(custom_wakeup_) {
        interrupt_lock_.unlock();
        (*custom_wakeup_)(custom_wakeup_data_);
        return true;
      }

      return false;
    }
  }

  void ThreadState::clear_waiter() {
    // TODO: Machine
    utilities::thread::SpinLock::LockGuard guard(_machine_->memory()->wait_lock());

    interrupt_with_signal_ = false;
    waiting_channel_ = nil<Channel>();
    waiting_object_ = cNil;
    custom_wakeup_ = 0;
    custom_wakeup_data_ = 0;
  }

  void ThreadState::wait_on_channel(STATE, Channel* chan) {
    std::lock_guard<locks::spinlock_mutex> guard(interrupt_lock_);

    thread()->sleep(state, cTrue);
    waiting_channel_ = chan;
  }

  void ThreadState::wait_on_custom_function(STATE, void (*func)(void*), void* data) {
    // TODO: Machine
    utilities::thread::SpinLock::LockGuard guard(_machine_->memory()->wait_lock());

    custom_wakeup_ = func;
    custom_wakeup_data_ = data;
  }

  void ThreadState::set_sleeping(STATE) {
    thread()->sleep(state, cTrue);
  }

  void ThreadState::clear_sleeping(STATE) {
    thread()->sleep(state, cFalse);
  }

  void ThreadState::reset_parked() {
    park_->reset_parked();
  }

  void ThreadState::register_raise(STATE, Exception* exc) {
    std::lock_guard<locks::spinlock_mutex> guard(interrupt_lock_);
    interrupted_exception_ = exc;
    set_check_local_interrupts();
  }

  void ThreadState::register_kill(STATE) {
    std::lock_guard<locks::spinlock_mutex> guard(interrupt_lock_);
    set_interrupt_by_kill();
    set_check_local_interrupts();
  }

  memory::VariableRootBuffers& ThreadState::current_root_buffers() {
    return variable_root_buffers();
  }

  void ThreadState::visit_objects(STATE, std::function<void (STATE, Object**)> f) {
    CallFrame* frame = call_frame_;

    while(frame) {
      f(state, reinterpret_cast<Object**>(&frame->lexical_scope_));
      f(state, reinterpret_cast<Object**>(&frame->compiled_code));

      if(frame->compiled_code) {
        intptr_t stack_size = frame->compiled_code->stack_size()->to_native();
        for(intptr_t i = 0; i < stack_size; i++) {
          f(state, &frame->stk[i]);
        }
      }

      f(state, reinterpret_cast<Object**>(&frame->top_scope_));

      BlockEnvironment* be = frame->block_env();
      f(state, reinterpret_cast<Object**>(&be));
      frame->set_block_env(be);

      Arguments* args = frame->arguments;

      if(!frame->inline_method_p() && args) {
        Object* recv = args->recv();
        f(state, &recv);
        args->set_recv(recv);

        Object* block = args->block();
        f(state, &block);
        args->set_block(block);

        if(Tuple* tup = args->argument_container()) {
          f(state, reinterpret_cast<Object**>(&tup));
          args->update_argument_container(tup);
        } else {
          Object** ary = args->arguments();
          for(uint32_t i = 0; i < args->total(); i++) {
            f(state, &ary[i]);
          }
        }
      }

      if(frame->scope && frame->compiled_code) {
        StackVariables* scope = frame->scope;

        f(state, reinterpret_cast<Object**>(&scope->self_));
        f(state, reinterpret_cast<Object**>(&scope->block_));
        f(state, reinterpret_cast<Object**>(&scope->module_));

        int locals = frame->compiled_code->machine_code()->number_of_locals;
        for(int i = 0; i < locals; i++) {
          Object* local = scope->get_local(i);
          f(state, &local);
          scope->set_local(i, local);
        }

        f(state, reinterpret_cast<Object**>(&scope->last_match_));
        f(state, reinterpret_cast<Object**>(&scope->parent_));
        f(state, reinterpret_cast<Object**>(&scope->on_heap_));
      }

      frame = frame->previous;
    }
  }

  void ThreadState::trace_objects(STATE, std::function<void (STATE, Object**)> f) {
    metrics()->checkpoints = checkpoints_;
    metrics()->stops = stops_;

    f(state, reinterpret_cast<Object**>(&waiting_channel_));
    f(state, reinterpret_cast<Object**>(&interrupted_exception_));
    f(state, reinterpret_cast<Object**>(&thread_));
    f(state, reinterpret_cast<Object**>(&fiber_));
    f(state, reinterpret_cast<Object**>(&waiting_object_));
    f(state, reinterpret_cast<Object**>(&unwind_state_));

    thca_->collect(state);

    CallFrame* frame = call_frame_;

    while(frame) {
      f(state, reinterpret_cast<Object**>(&frame->return_value));

      f(state, reinterpret_cast<Object**>(&frame->lexical_scope_));

      f(state, reinterpret_cast<Object**>(&frame->compiled_code));

      if(frame->compiled_code) {
        intptr_t stack_size = frame->compiled_code->stack_size()->to_native();
        for(intptr_t i = 0; i < stack_size; i++) {
          f(state, &frame->stk[i]);
        }
      }

      if(frame->multiple_scopes_p() && frame->top_scope_) {
        f(state, reinterpret_cast<Object**>(&frame->top_scope_));
      }

      if(BlockEnvironment* env = frame->block_env()) {
        f(state, reinterpret_cast<Object**>(&env));
        frame->set_block_env(env);
      }

      Arguments* args = frame->arguments;

      if(!frame->inline_method_p() && args) {
        Object* recv = args->recv();
        f(state, &recv);
        args->set_recv(recv);

        Object* block = args->block();
        f(state, &block);
        args->set_block(block);

        if(Tuple* tup = args->argument_container()) {
          f(state, reinterpret_cast<Object**>(&tup));
          args->update_argument_container(tup);
        } else {
          Object** ary = args->arguments();
          for(uint32_t i = 0; i < args->total(); i++) {
            f(state, &ary[i]);
          }
        }
      }

      if(frame->scope && frame->compiled_code) {
        // saw_variable_scope(frame, displace(frame->scope, offset));
        StackVariables* scope = frame->scope;

        f(state, reinterpret_cast<Object**>(&scope->self_));
        f(state, reinterpret_cast<Object**>(&scope->block_));
        f(state, reinterpret_cast<Object**>(&scope->module_));

        int locals = frame->compiled_code->machine_code()->number_of_locals;
        for(int i = 0; i < locals; i++) {
          Object* local = scope->get_local(i);
          f(state, &local);
          scope->set_local(i, local);
        }

        f(state, reinterpret_cast<Object**>(&scope->last_match_));
        f(state, reinterpret_cast<Object**>(&scope->parent_));
        f(state, reinterpret_cast<Object**>(&scope->on_heap_));
      }

      frame = frame->previous;
    }
  }
}