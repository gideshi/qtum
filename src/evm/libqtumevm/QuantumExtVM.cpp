#include "QuantumExtVM.h"
#include "libethereum/Executive.h"

using namespace dev;
using namespace dev::eth;

namespace{

    static unsigned const c_depthLimit = 1024;

    /// Upper bound of stack space needed by single CALL/CREATE execution. Set experimentally.
static size_t const c_singleExecutionStackSize = 6 * 1024;

/// Standard thread stack size.
static size_t const c_defaultStackSize =
#if defined(__linux)
	 8 * 1024 * 1024;
#elif defined(_WIN32)
	16 * 1024 * 1024;
#else
	512 * 1024; // OSX and other OSs
#endif

/// Stack overhead prior to allocation.
static size_t const c_entryOverhead = 128 * 1024;

/// On what depth execution should be offloaded to additional separated stack space.
static unsigned const c_offloadPoint = (c_defaultStackSize - c_entryOverhead) / c_singleExecutionStackSize;

void goOnOffloadedStack(Executive& _e, OnOpFunc const& _onOp)
{
	// Set new stack size enouth to handle the rest of the calls up to the limit.
	boost::thread::attributes attrs;
	attrs.set_stack_size((c_depthLimit - c_offloadPoint) * c_singleExecutionStackSize);

	// Create new thread with big stack and join immediately.
	// TODO: It is possible to switch the implementation to Boost.Context or similar when the API is stable.
	boost::exception_ptr exception;
	boost::thread{attrs, [&]{
		try
		{
			_e.go(_onOp);
		}
		catch (...)
		{
			exception = boost::current_exception(); // Catch all exceptions to be rethrown in parent thread.
		}
	}}.join();
	if (exception)
		boost::rethrow_exception(exception);
}

void go(unsigned _depth, Executive& _e, OnOpFunc const& _onOp)
{
	// If in the offloading point we need to switch to additional separated stack space.
	// Current stack is too small to handle more CALL/CREATE executions.
	// It needs to be done only once as newly allocated stack space it enough to handle
	// the rest of the calls up to the depth limit (c_depthLimit).

	if (_depth == c_offloadPoint)
	{
		cnote << "Stack offloading (depth: " << c_offloadPoint << ")";
		goOnOffloadedStack(_e, _onOp);
	}
	else
		_e.go(_onOp);
}

} // anonymous namespace


bool QuantumExtVM::call(CallParameters& _p)
{
	Executive e(m_s, envInfo(), m_sealEngine, depth + 1);

	if (!e.call(_p, gasPrice, origin))
	{
		go(depth, e, _p.onOp);
		e.accrueSubState(sub);
	}
	_p.gas = e.gas();

	return !e.excepted();
}

h160 QuantumExtVM::create(u256 _endowment, u256& io_gas, bytesConstRef _code, OnOpFunc const& _onOp)
{
	// Increment associated nonce for sender.
	m_s.noteSending(myAddress);

	Executive e(m_s, envInfo(), m_sealEngine, depth + 1);
	if (!e.create(myAddress, _endowment, gasPrice, io_gas, _code, origin))
	{
		go(depth, e, _onOp);
		e.accrueSubState(sub);
	}
	io_gas = e.gas();

	return e.newAddress();
}