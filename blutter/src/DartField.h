#pragma once
#include <string>
#include "DartTypes.h"

class DartClass;

class DartField
{
public:
	DartField(const DartClass& cls, dart::FieldPtr ptr);
	DartField(const DartClass& cls, uint32_t offset, DartAbstractType* type, std::string name="");
	DartField() = delete;
	DartField(const DartField&) = delete;
	DartField(DartField&&) = delete;
	DartField& operator=(const DartField&) = delete;

	void Print(std::ostream& of) const;

	dart::FieldPtr Ptr() const { return ptr; }
	const std::string& Name() const { return name; }
	std::string FullName() const;
	uint32_t Offset() const { return offset; }
	bool IsStatic() const { return is_static; }
	bool IsLate() const { return is_late; }
	bool IsFinal() const { return is_final; }
	bool IsConst() const { return is_const; }

	DartAbstractType* Type() const { return type; }
	void SetType(DartAbstractType* type) { this->type = type; }

	const DartClass& cls;

private:
	DartAbstractType* type;
	dart::FieldPtr ptr;
	dart::AbstractTypePtr typePtr;
	std::string typeName;
	std::string name;
	uint32_t offset;
	bool is_static;
	bool is_late;
	bool is_final;
	bool is_const;
	//bool is_covariant;
};

