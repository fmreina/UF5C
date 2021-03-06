// EPOS Thread Abstraction Implementation

#include <system/kmalloc.h>
#include <machine.h>
#include <thread.h>
#include <display.h>
#include <utility/ostream.h>
#include <synchronizer.h>

using namespace EPOS;
OStream cout2;

// This_Thread class attributes
__BEGIN_UTIL
bool This_Thread::_not_booting;
__END_UTIL

__BEGIN_SYS

// Class attributes
Scheduler_Timer * Thread::_timer;

Thread* volatile Thread::_running;
Thread::Queue Thread::_ready;
Thread::Queue Thread::_suspended;
unsigned int Thread::_threadCount = 0;

// Methods
void Thread::constructor_prolog(unsigned int stack_size)
{
    lock();

    _stack = reinterpret_cast<char *>(kmalloc(stack_size));
}


void Thread::constructor_epilog(const Log_Addr & entry, unsigned int stack_size)
{
    _threadCount++;

    db<Thread>(TRC) << "Thread(entry=" << entry
                    << ",state=" << _state
                    << ",priority=" << _link.rank()
                    << ",threadCount=" << _threadCount
                    << ",stack={b=" << reinterpret_cast<void *>(_stack)
                    << ",s=" << stack_size
                    << "},context={b=" << _context
                    << "," << *_context << "}) => " << this << endl;

    switch(_state) {
        case RUNNING: break;
        case SUSPENDED: _suspended.insert(&_link); break;
        default: _ready.insert(&_link);
    }

    unlock();
}


Thread::~Thread()
{
    lock();

    db<Thread>(TRC) << "~Thread(this=" << this
                    << ",state=" << _state
                    << ",priority=" << _link.rank()
                    << ",threadCount=" << _threadCount
                    << ",stack={b=" << reinterpret_cast<void *>(_stack)
                    << ",context={b=" << _context
                    << "," << *_context << "})" << endl;

    if(_synchronizer==0){
      _synchronizer.notifyDeletedThread(this);
    }

    // Confirma que ninguém está esperando está thread
    assert(_joining.empty());
    // Se a thread deletada está esperando outra terminar, é removida da lista de joining da outra thread
    if (_joined)
        _joined->_joining.remove(&_link);

    _ready.remove(this);
    _suspended.remove(this);

    if(_state != FINISHING){
      _threadCount--;
    }

    unlock();

    kfree(_stack);
}

int Thread::join()
{
    lock();

    db<Thread>(TRC) << "Thread::join(this=" << this << ",state=" << _state << ")" << endl;

    // Display::position(20,0);
    // int count = 1;
    // while(_state != FINISHING){
        // Display::position(20+count,0);
        // cout2<<"Rodou:"<<count;
        // count++;
        //yield(); // implicit unlock()

    //Evitar que thread possa dar join em si mesma
    assert(this != _running);

    //Evitar que duas threads façam chamadas cíclicas
    assert(this->joined != _running);

    if(_state != FINISHING){
        _running->_joined = this;

        Thread* previous = _running;
        previous->_state = WAITING;
        _joining.insert(&previous->_link);

        _running = previous->nextThread();
        _running->_state = RUNNING;
        Thread::dispatch(previous, _running);
    }else
        unlock();

    return *reinterpret_cast<int *>(_stack);
}


void Thread::pass()
{
    lock();

    db<Thread>(TRC) << "Thread::pass(this=" << this << ")" << endl;

    Thread * prev = _running;
    prev->_state = READY;
    _ready.insert(&prev->_link);

    _ready.remove(this);
    _state = RUNNING;
    _running = this;

    dispatch(prev, this);

    unlock();
}


void Thread::suspend()
{
    lock();

    db<Thread>(TRC) << "Thread::suspend(this=" << this << ")" << endl;

    if(_running != this)
        _ready.remove(this);

    _state = SUSPENDED;
    _suspended.insert(&_link);

    if(_running == this) {
       //&& !_ready.empty()

        _running = _ready.remove()->object();
        _running->_state = RUNNING;

        dispatch(this, _running);
    } //else
        //idle(); // implicit unlock()

    unlock();
}


void Thread::resume()
{
    lock();

    db<Thread>(TRC) << "Thread::resume(this=" << this << ")" << endl;

   _suspended.remove(this);
   _state = READY;
   _ready.insert(&_link);

   unlock();
}

void Thread::notifyNewReady(Thread * tReady)
{
    lock();

    db<Thread>(TRC) << "Thread::notifyNewReady(tready = " << tReady << ")" << endl;

    tReady->_state = READY;
    _ready.insert(&tReady->_link);

    if(preemptive){
        reschedule();
    }

    unlock();
}


// Class methods
void Thread::yield()
{
    lock();

    db<Thread>(TRC) << "Thread::yield(running=" << _running << ")" << endl;

    //if(!_ready.empty()) {
    Thread * prev = _running;
    prev->_state = READY;
    _ready.insert(&prev->_link);

    _running = _ready.remove()->object();
    _running->_state = RUNNING;

    dispatch(prev, _running);
    //} else
    //    idle();

    unlock();
}

void Thread::wakeup_joiners()
{
    lock();

    db<Thread>(TRC) << "Thread::wakeup_joiners(this=" << this << ")" << endl;

    Thread * joiner;

    while(!_joining.empty()) {
        joiner = _joining.remove()->object();
        notifyNewReady(joiner); // implicit unlock()
    }

    unlock();
}


void Thread::exit(int status)
{
    lock();

    db<Thread>(TRC) << "Thread::exit(status=" << status << ") [running=" << running() << "]" << endl;

    *reinterpret_cast<int *>(_running->_stack) = status;
    _running->wakeup_joiners(); // implicit unlock();

    lock();

    //while(_ready.empty() && !_suspended.empty())
    //    idle(); // implicit unlock();

    //lock();

    //if(!_ready.empty()) {
    Thread * prev = _running;
    prev->_state = FINISHING;
    // *reinterpret_cast<int *>(_running->_stack) = status;
    _threadCount--;

    _running = _ready.remove()->object();
    _running->_state = RUNNING;

    dispatch(prev, _running);
    //} else {
    //    db<Thread>(WRN) << "The last thread in the system has exited!" << endl;
    //    if(reboot) {
    //        db<Thread>(WRN) << "Rebooting the machine ..." << endl;
    //        Machine::reboot();
    //    } else {
    //        db<Thread>(WRN) << "Halting the CPU ..." << endl;
    //        CPU::halt();
    //    }
    //}

    unlock();
}


void Thread::reschedule()
{
    yield();
}


void Thread::time_slicer(const IC::Interrupt_Id & i)
{
    reschedule();
}


void Thread::dispatch(Thread * prev, Thread * next)
{
    if(prev != next) {
        if(prev->_state == RUNNING)
            prev->_state = READY;
        next->_state = RUNNING;

        db<Thread>(TRC) << "Thread::dispatch(prev=" << prev << ",next=" << next << ")" << endl;
        db<Thread>(INF) << "prev={" << prev << ",ctx=" << *prev->_context << "}" << endl;
        db<Thread>(INF) << "next={" << next << ",ctx=" << *next->_context << "}" << endl;

        CPU::switch_context(&prev->_context, next->_context);
    }

    unlock();
}


int Thread::idle()
{
    db<Thread>(TRC) << "Thread::idle()" << endl;

    //db<Thread>(INF) << "There are no runnable threads at the moment!" << endl;
    //db<Thread>(INF) << "Halting the CPU ..." << endl;

    //CPU::int_enable();
    //CPU::halt();

    while(_threadCount > 1) {
        db<Thread>(TRC) << "Thread::idle(thread_count=" << _threadCount << ")" << endl;

        db<Thread>(INF) << "There are no runnable threads at the moment!" << endl;
        db<Thread>(INF) << "Halting the CPU ..." << endl;

        CPU::int_enable();
        CPU::halt();
    }

    db<Thread>(WRN) << "The last thread in the system has exited!" << endl;
    if(reboot) {
        db<Thread>(WRN) << "Rebooting the machine ..." << endl;
        Machine::reboot();
    } else {
        db<Thread>(WRN) << "Halting the CPU ..." << endl;
        CPU::halt();
    }

    return 0;
}

__END_SYS

// Id forwarder to the spin lock
__BEGIN_UTIL
unsigned int This_Thread::id()
{
    return _not_booting ? reinterpret_cast<volatile unsigned int>(Thread::self()) : Machine::cpu_id() + 1;
}
__END_UTIL
