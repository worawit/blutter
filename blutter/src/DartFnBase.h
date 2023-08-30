#pragma once
#include <stdint.h>
#include <string>

// forward declaration
class DartFunction;
class DartStub;

class DartFnBase
{
public:
	// For setting current library loaded address
	// because the Dart code and global data are independent (access through Object Pool)
	// so we can set a code address to be as if the library is loaded at 0 (to be same for every run)
	static void SetLibBase(intptr_t addr) { lib_base = addr; }

	DartFnBase(const DartFnBase&) = default;
	DartFnBase(DartFnBase&&) = delete;
	DartFnBase& operator=(const DartFnBase&) = delete;
	virtual ~DartFnBase() {}

	uint64_t Address() const { return ep_addr; }
	uint64_t MemAddress() const { return ep_addr + lib_base; }
	virtual int64_t Size() const { return size; }
	virtual uint64_t AddressEnd() const { return ep_addr + Size(); }
	bool ContainsAddress(uint64_t addr) { return addr >= Address() && addr < AddressEnd(); }

	virtual std::string FullName() const { return name; }
	virtual std::string Name() const { return name; }
	// TODO: use dart type to support function type and type parameters ()
	virtual uint32_t ReturnType() const { return dart::kIllegalCid; }
	virtual bool IsStub() const { return false; }

	DartFunction* AsFunction() { ASSERT(!IsStub()); return reinterpret_cast<DartFunction*>(this); }
	DartStub* AsStub() { ASSERT(IsStub()); return reinterpret_cast<DartStub*>(this); }

protected:
	DartFnBase() : ep_addr(0), size(0) {}
	DartFnBase(uint64_t addr, int64_t size, std::string name) : ep_addr(addr), size(size), name(std::move(name)) {}

	uint64_t ep_addr;
	int64_t size;
	std::string name;

	static intptr_t lib_base;
};