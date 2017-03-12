#ifndef GVARIANT_H
#define GVARIANT_H

#include "cpgf/gtypelist.h"
#include "cpgf/gerrorcode.h"
#include "cpgf/gexception.h"
#include "cpgf/ginterface.h"
#include "cpgf/gcompiler.h"
#include "cpgf/gapiutil.h"
#include "cpgf/gmetatype.h"

#include <type_traits>
#include <cstdint>
#include <memory>

namespace xcpgf {

enum class GVariantType : uint16_t {
	vtEmpty = 0,
	vtVoid = 1,

	vtFundamentalBegin = 2,

	vtIntegerBegin = vtFundamentalBegin, // 2
	vtBool = vtIntegerBegin,
	vtChar = vtIntegerBegin + 1, vtWchar = vtIntegerBegin + 2,
	vtSignedChar = vtIntegerBegin + 3, vtUnsignedChar = vtIntegerBegin + 4,
	vtSignedShort = vtIntegerBegin + 5, vtUnsignedShort = vtIntegerBegin + 6,
	vtSignedInt = vtIntegerBegin + 7, vtUnsignedInt = vtIntegerBegin + 8,
	vtSignedLong = vtIntegerBegin + 9, vtUnsignedLong = vtIntegerBegin + 10,
	vtSignedLongLong = vtIntegerBegin + 11, vtUnsignedLongLong = vtIntegerBegin + 12,
	vtIntegerEnd = vtUnsignedLongLong,

	vtRealBegin = vtIntegerEnd + 1, // 15
	vtFloat = vtRealBegin, vtDouble = vtRealBegin + 2, vtLongDouble = vtRealBegin + 3,
	vtRealEnd = vtLongDouble,

	vtFundamentalEnd = vtLongDouble,

	vtObject = 31, // is equivalent to unkown type
	
	// special types
	vtInterfaceBegin = 32,
	vtShadow = vtInterfaceBegin,
	vtString = vtInterfaceBegin + 1,
	vtWideString = vtInterfaceBegin + 2,
	vtInterface = vtInterfaceBegin + 3, // IObject *
	vtTypedVar = vtInterfaceBegin + 4,
	vtInterfaceEnd = vtTypedVar,

	vtUserBegin = 0xff,
	vtUserEnd = 0x0fff,

	vtMask = 0x0fff,

	// when there are both pointer and reference, it's always reference to pointer since we can't have pointer to reference
	byPointer = 0x1000,
	byLvalueReference = 0x2000,
	byRvalueReference = 0x4000,
	
	maskByPointerAndReference = byPointer | byLvalueReference | byRvalueReference,
	maskByReference = byLvalueReference | byRvalueReference,
};

typedef cpgf::GTypeList<
	bool,
	char, wchar_t,
	signed char, unsigned char,
	signed short, unsigned short,
	signed int, unsigned int,
	signed long, unsigned long,
	signed long long, unsigned long long,
	float, double, long double
> FundamentalTypeList;

constexpr int FundamentalTypeStartType = (int)GVariantType::vtFundamentalBegin;

struct VariantTypeInfo
{
	int size;
};

extern VariantTypeInfo variantTypeInfo[];

#pragma pack(push, 1)
#pragma pack(1)
struct GVarTypeData
{
	uint16_t vt;
	uint8_t sizeAndPointers;
	uint8_t padding;
};
#pragma pack(pop)

typedef std::int64_t GVariantInteger;
typedef long double GVariantReal;

#pragma pack(push, 1)
#pragma pack(1)
struct GVariantData
{
	GVarTypeData typeData;

	union {
		std::int8_t valueInt8;
		std::int16_t valueInt16;
		std::int32_t valueInt32;
		std::int64_t valueInt64;
		GVariantInteger valueInt;
	
		float valueFloat;
		double valueDouble;
		long double valueLongDouble;
		GVariantReal valueReal;
	
		void * pointer;
		
		cpgf::IObject * valueInterface;

	};
};
#pragma pack(pop)

#include "private/gvariant_common_p.h"

#include "private/gvariant_create_p.h"

class GVariant
{
public:
	template <typename T>
	static GVariant create(const T & value)
	{
		GVariant v;
		variant_internal::deduceVariantType<T>(&v.data, value);
		return v;
	}
	
	GVariant() : data() {
	}

	template <typename T>
	GVariant(const T & value) : data()
	{
		variant_internal::deduceVariantType(&this->data, value);
	}

	GVariant(const GVariant & other) : data(other.data)
	{
		variant_internal::retainVariantData(this->data);
	}

	GVariant(GVariant && other) : data()
	{
		this->swap(other);
	}

	explicit GVariant(const GVariantData & otherData) : data(otherData)
	{
		variant_internal::retainVariantData(this->data);
	}

	explicit GVariant(GVariantData && otherData) : data(otherData)
	{
		otherData.typeData.vt = (uint16_t)GVariantType::vtEmpty;
	}

	GVariant & operator = (const GVariant & other)
	{
		this->data = other.data;
		variant_internal::retainVariantData(this->data);

		return *this;
	}

	GVariant & operator = (GVariant && other)
	{
		this->swap(other);

		return *this;
	}

	~GVariant() {
		variant_internal::releaseVariantData(this->data);
	}
	
	GVariantType getType() const {
		return (GVariantType)this->data.typeData.vt;
	}

	const GVariantData & refData() const {
		return this->data;
	}

	GVariantData & refData() {
		return this->data;
	}

	GVariantData takeData()
	{
		GVariantData result = this->data;
		this->data.typeData.vt = (uint16_t)GVariantType::vtEmpty;
		return result;
	}

	void reset()
	{
		variant_internal::releaseVariantData(this->data);
		this->data.typeData.vt = (uint16_t)GVariantType::vtEmpty;
	}

	void swap(GVariant & other) {
		using std::swap;

		swap(this->data, other.data);
	}

private:
	GVariantData data;
};

inline void swap(GVariant & a, GVariant & b)
{
	a.swap(b);
}

inline bool vtIsFundamental(const uint16_t vt)
{
	return vt >= (uint16_t)GVariantType::vtFundamentalBegin && vt <= (uint16_t)GVariantType::vtFundamentalEnd;
}

inline bool vtIsPointerOrReference(const uint16_t vt)
{
	return (vt & (int)GVariantType::maskByPointerAndReference) != 0;
}

inline bool vtIsPointer(const uint16_t vt)
{
	return (vt & (int)GVariantType::byPointer) != 0;
}

inline bool vtIsLvalueReference(const uint16_t vt)
{
	return (vt & (int)GVariantType::byLvalueReference) != 0;
}

inline bool vtIsRvalueReference(const uint16_t vt)
{
	return (vt & (int)GVariantType::byRvalueReference) != 0;
}

inline bool vtIsReference(const uint16_t vt)
{
	return (vt & (int)GVariantType::maskByReference) != 0;
}

inline bool vtIsTypedVar(const uint16_t vt) {
	return vt == (uint16_t)GVariantType::vtTypedVar;
}

inline GVariantType vtGetBaseType(const uint16_t vt)
{
	return (GVariantType)(vt & (uint16_t)GVariantType::vtMask);
}

inline GVariantType vtGetBaseType(const GVariantType vt)
{
	return vtGetBaseType((uint16_t)vt);
}

inline GVariantType vtGetBaseType(const GVarTypeData & data)
{
	return vtGetBaseType(data.vt);
}

inline int vtGetPointers(const GVarTypeData & data)
{
	return data.sizeAndPointers & 0x0f;
}

inline void vtSetPointers(GVarTypeData & data, unsigned int pointers)
{
	data.sizeAndPointers = static_cast<uint8_t>((data.sizeAndPointers & 0xf0) + pointers);
}

inline int vtGetSize(const GVarTypeData & data)
{
	return (data.sizeAndPointers >> 4) & 0x0f;
}

inline void vtSetSize(GVarTypeData & data, unsigned int size)
{
	data.sizeAndPointers = static_cast<uint8_t>(((size & 0x0f) << 4) | (data.sizeAndPointers & 0x0f));
}

inline void vtSetSizeAndPointers(GVarTypeData & data, unsigned int size, unsigned int pointer)
{
	data.sizeAndPointers = static_cast<uint8_t>(((size & 0x0f) << 4) | (pointer & 0x0f));
}

inline bool variantIsString(const GVariant & v)
{
	const GVariantData & data = v.refData();
	return data.typeData.vt == (uint16_t)GVariantType::vtString
		|| ((vtGetPointers(data.typeData) == 1 && vtGetBaseType(data.typeData) == GVariantType::vtChar));
}

inline bool variantIsWideString(const GVariant & v)
{
	const GVariantData & data = v.refData();
	return data.typeData.vt == (uint16_t)GVariantType::vtWideString
		|| ((vtGetPointers(data.typeData) == 1 && vtGetBaseType(data.typeData) == GVariantType::vtWchar));
}

struct VarantCastKeepConstRef {};
struct VarantCastCopyConstRef {};

#include "private/gvariant_from_p.h"

GVariant createStringVariant(const char * s);
GVariant createWideStringVariant(const wchar_t * s);
GVariant createTypedVariant(const GVariant & value, const cpgf::GMetaType & type);

GVariant getVariantRealValue(const GVariant & value);
cpgf::GMetaType getVariantRealMetaType(const GVariant & value);

GVariant pointerToObjectVariant(void * p);
GVariant objectToVariant(void * object);

// Convert a pointer to reference.
GVariant variantPointerToLvalueReference(const GVariant & p);

template <typename T, typename Policy = VarantCastKeepConstRef>
typename variant_internal::VariantCastResult<T, Policy>::Result fromVariant(const GVariant & value)
{
	using namespace variant_internal;

	auto vt = value.refData().typeData.vt;

	if(vtIsTypedVar(vt)) {
		return fromVariant<T>(getVariantRealValue(value));
	}
	else if(vtIsPointer(vt)) {
		if(vtIsLvalueReference(vt)) {
			return CastVariant_Pointer_LvalueReference<T, Policy>::cast(value);
		}
		else if(vtIsRvalueReference(vt)) {
			return CastVariant_Pointer_RvalueReference<T, Policy>::cast(value);
		}
		else {
			return CastVariant_Pointer<T, Policy>::cast(value);
		}
	}
	else if(vtIsLvalueReference(vt)) {
		return CastVariant_LvalueReference<T, Policy>::cast(value);
	}
	else if(vtIsRvalueReference(vt)) {
		return CastVariant_RvalueReference<T, Policy>::cast(value);
	}
	else {
		return CastVariant_Value<T, Policy>::cast(value);
	}
}


template <typename T, typename Policy = VarantCastKeepConstRef>
bool canFromVariant(const GVariant & value)
{
	using namespace variant_internal;

	auto vt = value.refData().typeData.vt;
	
	if(vtIsTypedVar(vt)) {
		return canFromVariant<T, Policy>(getVariantRealValue(value));
	}
	else if(vtIsPointer(vt)) {
		if(vtIsLvalueReference(vt)) {
			return CastVariant_Pointer_LvalueReference<T, Policy>::canCast(value);
		}
		else if(vtIsRvalueReference(vt)) {
			return CastVariant_Pointer_RvalueReference<T, Policy>::canCast(value);
		}
		else {
			return CastVariant_Pointer<T, Policy>::canCast(value);
		}
	}
	else if(vtIsLvalueReference(vt)) {
		return CastVariant_LvalueReference<T, Policy>::canCast(value);
	}
	else if(vtIsRvalueReference(vt)) {
		return CastVariant_RvalueReference<T, Policy>::canCast(value);
	}
	else {
		return CastVariant_Value<T, Policy>::canCast(value);
	}
}

template <typename T>
GVariant createVariant(const T & value, bool copyObject = false,
	typename std::enable_if<(std::is_copy_constructible<T>::value && (std::is_class<T>::value || std::is_union<T>::value))>::type * = 0
	)
{
	if(copyObject) {
		GVariant v;
		GVariantData & data = v.refData();

		data.typeData.vt = (uint16_t)GVariantType::vtShadow;
		vtSetSizeAndPointers(data.typeData, sizeof(void *), 0);
		data.valueInterface = new variant_internal::GVariantShadowObject<T>(value);

		return v;
	}
	else {
		return GVariant(value);
	}
}

template <typename T>
GVariant createVariant(const T & value, bool /*copyObject*/ = false,
	typename std::enable_if<! (std::is_copy_constructible<T>::value && (std::is_class<T>::value || std::is_union<T>::value))>::type * = 0
)
{
	return GVariant(value);
}

template <typename T>
GVariant copyVariantFromCopyable(const T & value)
{
	return createVariant(value, true);
}

inline void * objectAddressFromVariant(const GVariant & v)
{
	return fromVariant<void *>(v);
}


} //namespace cpgf


#endif

