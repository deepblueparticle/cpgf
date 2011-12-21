#include "cpgf/scriptbind/gv8bind.h"
#include "cpgf/gmetaclasstraveller.h"

#include "../pinclude/gbindcommon.h"
#include "../pinclude/gscriptbindapiimpl.h"

#include <vector>
#include <set>

using namespace std;
using namespace cpgf::bind_internal;
using namespace v8;


#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable:4996)
#endif


#define ENTER_V8() \
	char local_msg[256]; bool local_error = false; { \
	try {

#define LEAVE_V8(...) \
	} \
	catch(const GException & e) { strncpy(local_msg, e.getMessage(), 256); local_error = true; } \
	catch(...) { strcpy(local_msg, "Unknown exception occurred."); local_error = true; } \
	} if(local_error) { local_msg[255] = 0; v8::ThrowException(v8::String::New(local_msg)); } \
	__VA_ARGS__;


namespace cpgf {

namespace {

class GUserDataPool
{
private:
	typedef std::set<GScriptUserData *> ListType;

public:
	~GUserDataPool() {
		for(ListType::iterator it = this->userDataList.begin(); it != this->userDataList.end(); ++it) {
			delete *it;
		}
	}

	void addUserData(GScriptUserData * userData) {
		if(this->userDataList.find(userData) == this->userDataList.end()) {
			this->userDataList.insert(userData);
		}
	}

	void removeUserData(GScriptUserData * userData) {
		ListType::iterator it = this->userDataList.find(userData);
		if(it != this->userDataList.end()) {
			delete *it;
			this->userDataList.erase(it);
		}
	}


private:
	ListType userDataList;
};

GUserDataPool * getUserDataPool() {
	static GUserDataPool pool;
	return &pool;
}

class GMapItemClassData : public GMetaMapItemData
{
public:
	virtual ~GMapItemClassData() {
		this->functionTemplate.Dispose();
		this->functionTemplate.Clear();
	}

	void setTemplate(v8::Handle<v8::FunctionTemplate> newTemplate) {
		this->functionTemplate = v8::Persistent<v8::FunctionTemplate>::New(newTemplate);
	}

public:
	v8::Persistent<v8::FunctionTemplate> functionTemplate;
};

class GMapItemMethodData : public GMetaMapItemData
{
public:
	GMapItemMethodData() : userData(NULL) {
	}

	virtual ~GMapItemMethodData() {
		this->functionTemplate.Dispose();
		this->functionTemplate.Clear();
	}

	void setTemplate(v8::Handle<v8::FunctionTemplate> newTemplate) {
		this->functionTemplate = v8::Persistent<v8::FunctionTemplate>::New(newTemplate);
	}

	void setUserData(GExtendMethodUserData * userData) {
		this->userData = userData;
	}

	GExtendMethodUserData * getUserData() const {
		return this->userData;
	}

public:
	v8::Persistent<v8::FunctionTemplate> functionTemplate;
	GExtendMethodUserData * userData;
};

class GMapItemEnumData : public GMetaMapItemData
{
public:
	GMapItemEnumData() : userData(NULL) {
	}

	virtual ~GMapItemEnumData() {
		this->objectTemplate.Dispose();
		this->objectTemplate.Clear();
	}

	void setTemplate(v8::Handle<v8::ObjectTemplate> newTemplate) {
		this->objectTemplate = v8::Persistent<v8::ObjectTemplate>::New(newTemplate);
	}

	void setUserData(GEnumUserData * userData) {
		this->userData = userData;
	}

	GEnumUserData * getUserData() const {
		return this->userData;
	}

public:
	v8::Persistent<v8::ObjectTemplate> objectTemplate;
	GEnumUserData * userData;
};


class GV8ScriptBindingParam : public GScriptBindingParam
{
private:
	typedef GScriptBindingParam super;

public:
	GV8ScriptBindingParam(IMetaService * service, const GScriptConfig & config, GMetaMap * metaMap)
		: super(service, config, metaMap)
	{
	}

	virtual ~GV8ScriptBindingParam() {
		if(! this->objectTemplate.IsEmpty()) {
			this->objectTemplate.Dispose();
			this->objectTemplate.Clear();
		}
	}

	Handle<Object > getRawObject() {
		if(this->objectTemplate.IsEmpty()) {
			this->objectTemplate  = Persistent<ObjectTemplate>::New(ObjectTemplate::New());
			this->objectTemplate->SetInternalFieldCount(1);
		}

		return this->objectTemplate->NewInstance();
	}

private:
	Persistent<ObjectTemplate> objectTemplate;
};


void weakHandleCallback(v8::Persistent<v8::Value> object, void * parameter);
v8::Handle<v8::FunctionTemplate> createClassTemplate(GScriptBindingParam * param, const char * name, IMetaClass * metaClass);

v8::Handle<v8::Value> converterToV8(GScriptBindingParam * param, const GVariant & value, IMetaConverter * converter);

const char * signatureKey = "i_sig_cpgf";
const int signatureValue = 0x168feed;
const char * userDataKey = "i_userdata_cpgf";

template <typename T>
void setObjectSignature(T * object)
{
	(*object)->SetHiddenValue(v8::String::New(signatureKey), v8::Int32::New(signatureValue));
}

bool isValidObject(v8::Handle<v8::Value> object)
{
	if(object->IsObject() || object->IsFunction()) {
		Handle<Value> value = Handle<Object>::Cast(object)->GetHiddenValue(String::New(signatureKey));

		return !value.IsEmpty() && value->IsInt32() && value->Int32Value() == signatureValue;
	}
	else {
		return false;
	}
}

bool isGlobalObject(v8::Handle<v8::Value> object)
{
	if(object->IsObject() || object->IsFunction()) {
		return Handle<Object>::Cast(object)->InternalFieldCount () == 0
			|| Handle<Object>::Cast(object)->GetPointerFromInternalField(0) == NULL;
	}
	else {
		return false;
	}
}

GScriptDataType getV8Type(v8::Local<v8::Value> value, IMetaTypedItem ** typeItem)
{
	if(typeItem != NULL) {
		*typeItem = NULL;
	}

	if(value->IsNull()) {
		return sdtNull;
	}

	if(value->IsUndefined()) {
		return sdtNull;
	}

	if(value->IsBoolean()) {
		return sdtFundamental;
	}

	if(value->IsInt32() || value->IsNumber()) {
		return sdtFundamental;
	}

	if(value->IsString()) {
		return sdtString;
	}

	if(value->IsFunction() || value->IsObject()) {
		Local<Object> obj = value->ToObject();
		if(isValidObject(obj)) {
			if(obj->InternalFieldCount() == 0) {
				Handle<Value> value = obj->GetHiddenValue(v8::String::New(userDataKey));
				if(! value.IsEmpty()) {
					if(value->IsExternal()) {
						GScriptUserData * userData = static_cast<GScriptUserData *>(Handle<External>::Cast(value)->Value());
						switch(userData->getType()) {
							case udtClass:
								if(typeItem != NULL) {
									*typeItem = gdynamic_cast<GClassUserData *>(userData)->metaClass;
									(*typeItem)->addReference();
								}
								return sdtClass;

							case udtExtendMethod:
								return methodTypeToUserDataType(gdynamic_cast<GExtendMethodUserData *>(userData)->methodType);

							default:
								break;
						}
					}
				}

			}
			else {
				GScriptUserData * userData = static_cast<GScriptUserData *>(obj->GetPointerFromInternalField(0));
				if(userData != NULL) {
					switch(userData->getType()) {
						case udtClass: {
							GClassUserData * classData = gdynamic_cast<GClassUserData *>(userData);
							if(typeItem != NULL) {
								*typeItem = classData->metaClass;
								(*typeItem)->addReference();
							}
							if(classData->instance == NULL) {
								return sdtClass;
							}
							else {
								return sdtObject;
							}
						}

						break;

						case udtEnum:
							if(typeItem != NULL) {
								*typeItem = gdynamic_cast<GEnumUserData *>(userData)->metaEnum;
								(*typeItem)->addReference();
							}
							return sdtEnum;

					    case udtRaw:
					    	return sdtRaw;

						default:
						break;
					}
				}
			}
		}

		if(value->IsFunction()) {
			return sdtScriptMethod;
		}
		else {
			return sdtScriptObject;
		}
	}

	return sdtUnknown;
}

v8::Handle<v8::Value> objectToV8(GScriptBindingParam * param, void * instance, IMetaClass * metaClass, bool allowGC, ObjectPointerCV cv)
{
	if(instance == NULL) {
		return Handle<Value>();
	}

	GMetaMapClass * map = param->getMetaMap()->findClassMap(metaClass);
	GMapItemClassData * mapData = gdynamic_cast<GMapItemClassData *>(map->getData());
	Handle<FunctionTemplate> functionTemplate = mapData->functionTemplate;
	Persistent<Object> self = Persistent<Object>::New(functionTemplate->GetFunction()->NewInstance());

	GClassUserData * instanceUserData = new GClassUserData(param, metaClass, instance, true, allowGC, cv);
	getUserDataPool()->addUserData(instanceUserData);
	self.MakeWeak(instanceUserData, weakHandleCallback);

	self->SetPointerInInternalField(0, instanceUserData);
	setObjectSignature(&self);

	return self;
}

v8::Handle<v8::Value> rawToV8(GScriptBindingParam * param, const GVariant & value)
{
	GVariantType vt = value.getType();

	if(param->getConfig().allowAccessRawData() && variantIsRawData(vt)) {
		Persistent<Object> self = Persistent<Object>::New(gdynamic_cast<GV8ScriptBindingParam *>(param)->getRawObject());

		GRawUserData * instanceUserData = new GRawUserData(param, value);
		getUserDataPool()->addUserData(instanceUserData);
		self.MakeWeak(instanceUserData, weakHandleCallback);

		self->SetPointerInInternalField(0, instanceUserData);
		setObjectSignature(&self);

		return self;
	}

	return v8::Handle<v8::Value>();
}

v8::Handle<v8::Value> variantToV8(GScriptBindingParam * param, const GVariant & value, const GMetaType & type, bool allowGC, bool allowRaw)
{
	GVariantType vt = value.getType();

	if(vtIsEmpty(vt)) {
		return v8::Handle<v8::Value>();
	}

	if(vtIsBoolean(vt)) {
		return v8::Boolean::New(fromVariant<bool>(value));
	}

	if(vtIsInteger(vt)) {
		return v8::Integer::New(fromVariant<int>(value));
	}

	if(vtIsReal(vt)) {
		return v8::Number::New(fromVariant<double>(value));
	}

	if(canFromVariant<void *>(value) && objectAddressFromVariant(value) == NULL) {
		return v8::Null();
	}

	if(variantIsString(value)) {
		return v8::String::New(fromVariant<char *>(value));
	}

	if(! type.isEmpty() && type.getPointerDimension() <= 1) {
		GScopedInterface<IMetaTypedItem> typedItem(param->getService()->findTypedItemByName(type.getBaseName()));
		if(typedItem) {
			bool isReference = type.isReference();
			
			if(type.getPointerDimension() == 0 && !isReference) {
				GASSERT_MSG(!! metaIsClass(typedItem->getCategory()), "Unknown type");
				GASSERT_MSG(type.baseIsClass(), "Unknown type");

				IMetaClass * metaClass = gdynamic_cast<IMetaClass *>(typedItem.get());
				void * instance = metaClass->cloneInstance(objectAddressFromVariant(value));
				return objectToV8(param, instance, gdynamic_cast<IMetaClass *>(typedItem.get()), true, metaTypeToCV(type));
			}

			if(type.getPointerDimension() == 1 || isReference) {
				GASSERT_MSG(!! metaIsClass(typedItem->getCategory()), "Unknown type");

				return objectToV8(param, fromVariant<void *>(value), gdynamic_cast<IMetaClass *>(typedItem.get()), allowGC, metaTypeToCV(type));
			}
		}
	}

	if(allowRaw) {
		return rawToV8(param, value);
	}

	return v8::Handle<v8::Value>();
}

void * v8ToObject(v8::Handle<v8::Value> value, GMetaType * outType)
{
	if(isValidObject(value)) {
		GScriptUserData * userData = static_cast<GScriptUserData *>(Handle<Object>::Cast(value)->GetPointerFromInternalField(0));
		if(userData != NULL && userData->getType() == udtClass) {
			GClassUserData * classData = gdynamic_cast<GClassUserData *>(userData);
			if(outType != NULL) {
				GMetaTypeData typeData;
				classData->metaClass->getMetaType(&typeData);
				metaCheckError(classData->metaClass);
				*outType = GMetaType(typeData);
			}

			return classData->instance;
		}
	}

	return NULL;
}

GMetaVariant userDataToVariant(v8::Handle<v8::Value> value)
{
	if(value->IsFunction() || value->IsObject()) {
		Local<Object> obj = value->ToObject();
		if(isValidObject(obj)) {
			GScriptUserData * userData = static_cast<GScriptUserData *>(obj->GetPointerFromInternalField(0));
			if(userData != NULL) {
				return userDataToVariant(userData);
			}
		}
	}
	
	return GMetaVariant();
}

GMetaVariant v8ToVariant(v8::Handle<v8::Value> value)
{
	if(value.IsEmpty()) {
		return GMetaVariant();
	}

	if(value->IsBoolean()) {
		return value->BooleanValue();
	}

	if(value->IsInt32()) {
		return value->Int32Value();
	}

	if(value->IsNull()) {
		return (void *)0;
	}

	if(value->IsNumber()) {
		return value->NumberValue();
	}

	if(value->IsString()) {
		v8::String::AsciiValue s(value);
		return GMetaVariant(createStringVariant(*s), createMetaType<char *>());
	}

	if(value->IsUint32()) {
		return value->Uint32Value();
	}

	if(value->IsFunction() || value->IsObject()) {
		return userDataToVariant(value);
	}

	return GMetaVariant();
}

void weakHandleCallback(v8::Persistent<v8::Value> object, void * parameter)
{
	GScriptUserData * userData = static_cast<GScriptUserData *>(parameter);

	if(userData != NULL) {
		getUserDataPool()->removeUserData(userData);
	}

	object.Dispose();
	object.Clear();
}

void loadMethodParameters(const v8::Arguments & args, GScriptBindingParam * param, GVariantData * outputParams)
{
	(void)param;

	for(int i = 0; i < args.Length(); ++i) {
		outputParams[i] = v8ToVariant(args[i]).takeData().varData;
	}
}

void loadMethodParamTypes(const v8::Arguments & args, GBindDataType * outputTypes)
{
	for(int i = 0; i < args.Length(); ++i) {
		IMetaTypedItem * typeItem;
		outputTypes[i].dataType = getV8Type(args[i], &typeItem);
		outputTypes[i].typeItem.reset(typeItem);
	}
}

void loadCallableParam(const v8::Arguments & args, GScriptBindingParam * param, InvokeCallableParam * callableParam)
{
	if(args.Length() > REF_MAX_ARITY) {
		raiseCoreException(Error_ScriptBinding_CallMethodWithTooManyParameters);
	}

	callableParam->paramCount = args.Length();
	loadMethodParameters(args, param, callableParam->paramsData);
	loadMethodParamTypes(args, callableParam->paramsType);
}

v8::Handle<v8::Value> accessibleGet(v8::Local<v8::String> prop, const v8::AccessorInfo & info)
{
	(void)prop;

	ENTER_V8()

	GAccessibleUserData * userData = static_cast<GAccessibleUserData *>(Local<External>::Cast(info.Data())->Value());

	GMetaType type;
	GVariant result = getAccessibleValueAndType(userData->instance, userData->accessible, &type, false);

	v8::Handle<v8::Value> v;
	v = variantToV8(userData->getParam(), result, type, false, false);
	if(v.IsEmpty()) {
		GScopedInterface<IMetaConverter> converter(userData->accessible->createConverter());
		v = converterToV8(userData->getParam(), result, converter.get());
	}
	if(v.IsEmpty()) {
		v = rawToV8(userData->getParam(), result);
	}
	return v;
	
	LEAVE_V8(return Handle<Value>())
}

void accessibleSet(v8::Local<v8::String> prop, v8::Local<v8::Value> value, const v8::AccessorInfo & info)
{
	(void)prop;

	ENTER_V8()

	GAccessibleUserData * userData = static_cast<GAccessibleUserData *>(Local<External>::Cast(info.Data())->Value());

	GMetaVariant v = v8ToVariant(value);
	metaSetValue(userData->accessible, userData->instance, v.getValue());
	
	LEAVE_V8()
}

void doBindAccessible(GScriptBindingParam * param, v8::Local<v8::Object> container,
	const char * name, void * instance, IMetaAccessible * accessible)
{
	GAccessibleUserData * userData = new GAccessibleUserData(param, instance, accessible);
	getUserDataPool()->addUserData(userData);
	Persistent<External> data = Persistent<External>::New(External::New(userData));
	data.MakeWeak(userData, weakHandleCallback);

	container->SetAccessor(String::New(name), &accessibleGet, &accessibleSet, data);
}

v8::Handle<v8::Value> converterToV8(GScriptBindingParam * param, const GVariant & value, IMetaConverter * converter)
{
	if(converter != NULL) {
		if(converter->canToCString()) {
			gapi_bool needFree;

			GScopedInterface<IMemoryAllocator> allocator(param->getService()->getAllocator());
			const char * s = converter->toCString(objectAddressFromVariant(value), &needFree, allocator.get());

			if(s != NULL) {
				Handle<Value> value = String::New(s);

				if(needFree) {
					allocator->free((void *)s);
				}

				return value;
			}
		}
	}

	return Handle<Value>();
}

v8::Handle<v8::Value> methodResultToV8(GScriptBindingParam * param, IMetaCallable * callable, InvokeCallableResult * result)
{
	if(result->resultCount > 0) {
		GMetaTypeData typeData;

		callable->getResultType(&typeData);
		metaCheckError(callable);

		GVariant value = GVariant(result->resultData);
		v8::Handle<v8::Value> v;
		v = variantToV8(param, value, GMetaType(typeData), !! callable->isResultTransferOwnership(), false);
		if(v.IsEmpty()) {
			GScopedInterface<IMetaConverter> converter(callable->createResultConverter());
			v = converterToV8(param, value, converter.get());
		}
		if(v.IsEmpty()) {
			v = rawToV8(param, value);
		}
		if(v.IsEmpty()) {
			raiseCoreException(Error_ScriptBinding_FailVariantToScript);
		}
		return v;

	}

	return v8::Handle<v8::Value>();
}

v8::Handle<v8::Value> callbackMethodList(const v8::Arguments & args)
{
	ENTER_V8()

	bool isGlobal = isGlobalObject(args.Holder());

	if(!isGlobal && !isValidObject(args.Holder())) {
		raiseCoreException(Error_ScriptBinding_AccessMemberWithWrongObject);
	}

	GClassUserData * userData = NULL;
	
	if(!isGlobal) {
		userData = static_cast<GClassUserData *>(args.Holder()->GetPointerFromInternalField(0));
	}

	Local<External> data = Local<External>::Cast(args.Data());
	GExtendMethodUserData * namedUserData = static_cast<GExtendMethodUserData *>(data->Value());

	InvokeCallableParam callableParam;
	loadCallableParam(args, namedUserData->getParam(), &callableParam);

	void * instance = NULL;
	if(userData != NULL) {
		instance = userData->instance;
	}

	GScopedInterface<IMetaList> methodList;
	if(namedUserData->metaClass == NULL) {
		methodList.reset(namedUserData->methodList);
		methodList->addReference();
	}
	else {
		methodList.reset(createMetaList());
		loadMethodList(methodList.get(), namedUserData->getParam()->getMetaMap(), userData == NULL? namedUserData->metaClass : userData->metaClass,
			instance,  userData, namedUserData->name.c_str());
	}

	int maxRank = -1;
	int maxRankIndex = -1;

	uint32_t count = methodList->getCount();

	for(uint32_t i = 0; i < count; ++i) {
		GScopedInterface<IMetaCallable> meta(gdynamic_cast<IMetaCallable *>(methodList->getAt(i)));
		if(isGlobal || allowInvokeCallable(userData, meta.get())) {
			int rank = rankCallable(namedUserData->getParam()->getService(), meta.get(), &callableParam);
			if(rank > maxRank) {
				maxRank = rank;
				maxRankIndex = static_cast<int>(i);
			}
		}
	}

	if(maxRankIndex >= 0) {
		InvokeCallableResult result;
		GScopedInterface<IMetaCallable> callable(gdynamic_cast<IMetaCallable *>(methodList->getAt(maxRankIndex)));
		doInvokeCallable(methodList->getInstanceAt(static_cast<uint32_t>(maxRankIndex)), callable.get(), callableParam.paramsData, callableParam.paramCount, &result);
		return methodResultToV8(namedUserData->getParam(), callable.get(), &result);
	}

	raiseCoreException(Error_ScriptBinding_CantFindMatchedMethod);
	return v8::Handle<v8::Value>();

	LEAVE_V8(return Handle<Value>())
}

v8::Handle<v8::FunctionTemplate> createMethodTemplate(GScriptBindingParam * param, IMetaClass * metaClass, bool isGlobal, IMetaList * methodList,
	const char * name, v8::Handle<v8::FunctionTemplate> classTemplate, GUserDataMethodType methodType, GExtendMethodUserData ** outUserData)
{
	(void)classTemplate;

	GExtendMethodUserData * userData = new GExtendMethodUserData(param, metaClass, methodList, name, methodType);
	if(outUserData != NULL) {
		*outUserData = userData;
	}

	Persistent<External> data = Persistent<External>::New(External::New(userData));
	getUserDataPool()->addUserData(userData);
	data.MakeWeak(NULL, weakHandleCallback);

	Handle<FunctionTemplate> functionTemplate;
	if(metaClass == NULL || isGlobal) {
		functionTemplate = FunctionTemplate::New(callbackMethodList, data);
	}
	else {
		functionTemplate = FunctionTemplate::New(callbackMethodList, data, Signature::New(classTemplate));
	}
	functionTemplate->SetClassName(String::New(name));

	Local<Function> func = functionTemplate->GetFunction();
	setObjectSignature(&func);
	GExtendMethodUserData * funcUserData = new GExtendMethodUserData(param, metaClass, methodList, name, methodType);
	Persistent<External> funcData = Persistent<External>::New(External::New(funcUserData));
	getUserDataPool()->addUserData(funcUserData);
	funcData.MakeWeak(funcUserData, weakHandleCallback);
	func->SetHiddenValue(v8::String::New(userDataKey), funcData);

	return functionTemplate;
}

v8::Handle<v8::Value> namedEnumGetter(v8::Local<v8::String> prop, const v8::AccessorInfo & info)
{
	ENTER_V8()

	GEnumUserData * userData = static_cast<GEnumUserData *>(info.Holder()->GetPointerFromInternalField(0));
	IMetaEnum * metaEnum = userData->metaEnum;
	v8::String::AsciiValue name(prop);
	int32_t index = metaEnum->findKey(*name);
	if(index >= 0) {
		return variantToV8(userData->getParam(), metaGetEnumValue(metaEnum, index), GMetaType(), true, false);
	}

	raiseCoreException(Error_ScriptBinding_CantFindEnumKey, *name);
	return v8::Handle<v8::Value>();

	LEAVE_V8(return Handle<Value>())
}

v8::Handle<v8::Value> namedEnumSetter(v8::Local<v8::String> prop, v8::Local<v8::Value> value, const v8::AccessorInfo & info)
{
	(void)prop;
	(void)value;
	(void)info;

	ENTER_V8()

	raiseCoreException(Error_ScriptBinding_CantAssignToEnumMethodClass);
	
	return v8::Handle<v8::Value>();

	LEAVE_V8(return v8::Handle<v8::Value>())
}

v8::Handle<v8::ObjectTemplate> createEnumTemplate(GScriptBindingParam * param, IMetaEnum * metaEnum,
	const char * name, GEnumUserData ** outUserData)
{
	(void)name;

	GEnumUserData * userData = new GEnumUserData(param, metaEnum);
	getUserDataPool()->addUserData(userData);
	if(outUserData != NULL) {
		*outUserData = userData;
	}

	Handle<ObjectTemplate> objectTemplate = ObjectTemplate::New();
	objectTemplate->SetInternalFieldCount(1);
	objectTemplate->SetNamedPropertyHandler(&namedEnumGetter, &namedEnumSetter);

	return objectTemplate;
}

v8::Handle<v8::Value> getNamedMember(GClassUserData * userData, const char * name)
{
	GMetaClassTraveller traveller(userData->metaClass, userData->instance);

	void * instance = NULL;
	IMetaClass * outDerived;

	for(;;) {
		GScopedInterface<IMetaClass> metaClass(traveller.next(&instance, &outDerived));
		GScopedInterface<IMetaClass> derived(outDerived);

		if(!metaClass) {
			break;
		}

		GMetaMapClass * mapClass = userData->getParam()->getMetaMap()->findClassMap(metaClass.get());
		if(mapClass == NULL) {
			continue;
		}
		GMetaMapItem * mapItem = mapClass->findItem(name);
		if(mapItem == NULL) {
			continue;
		}

		switch(mapItem->getType()) {
			case mmitField:
			case mmitProperty: {
				GScopedInterface<IMetaAccessible> data(gdynamic_cast<IMetaAccessible *>(mapItem->getItem()));
				if(allowAccessData(userData, data.get())) {
					GMetaType type;
					GVariant result = getAccessibleValueAndType(instance, data.get(), &type, userData->cv == opcvConst);
					Handle<Value> v = variantToV8(userData->getParam(), result, type, false, false);
					if(v.IsEmpty()) {
						GScopedInterface<IMetaConverter> converter(data->createConverter());
						v = converterToV8(userData->getParam(), result, converter.get());
					}
					if(v.IsEmpty()) {
						v = rawToV8(userData->getParam(), result);
					}
					if(v.IsEmpty()) {
						raiseCoreException(Error_ScriptBinding_FailVariantToScript);
					}
					return v;
				}
			}
			   break;

			case mmitMethod:
			case mmitMethodList: {
				GMapItemMethodData * data = gdynamic_cast<GMapItemMethodData *>(mapItem->getData());
				if(data == NULL) {
					GScopedInterface<IMetaList> methodList(createMetaList());
					loadMethodList(&traveller, methodList.get(), userData->getParam()->getMetaMap(), mapItem, instance, userData, name, true);

					data = new GMapItemMethodData;
					mapItem->setData(data);
					GExtendMethodUserData * newUserData;

					// select the class to bind to the method (i.e, to call the method, an object must be the class or the class' derived)
					// that to ensure v8::Arguments::Holder is correct
					GScopedInterface<IMetaClass> boundClass;
					if(!derived) {
						boundClass.reset(metaClass.get());
						boundClass->addReference();
					}
					else {
						if(derived->getBaseCount() > 0) {
							// always choose first base because we only support single inheritance in JS
							boundClass.reset(derived->getBaseClass(0));
						}
						else {
							boundClass.reset(derived.get());
							boundClass->addReference();
						}
					}

					GMetaMapClass * baseMapClass = userData->getParam()->getMetaMap()->findClassMap(boundClass.get());
					data->setTemplate(createMethodTemplate(userData->getParam(), userData->metaClass, userData->instance == NULL, methodList.get(), name,
						gdynamic_cast<GMapItemClassData *>(baseMapClass->getData())->functionTemplate, udmtInternal, &newUserData));
					newUserData->baseInstance = userData->instance;
					newUserData->name = name;
					data->setUserData(newUserData);
				}

				return data->functionTemplate->GetFunction();
			}

			case mmitEnum:
				if(! userData->isInstance || userData->getParam()->getConfig().allowAccessEnumTypeViaInstance()) {
					GMapItemEnumData * data = gdynamic_cast<GMapItemEnumData *>(mapItem->getData());
					if(data == NULL) {
						data = new GMapItemEnumData;
						mapItem->setData(data);
						GEnumUserData * newUserData;
						GScopedInterface<IMetaEnum> metaEnum(gdynamic_cast<IMetaEnum *>(mapItem->getItem()));
						data->setTemplate(createEnumTemplate(userData->getParam(), metaEnum.get(), name, &newUserData));
						data->setUserData(newUserData);
					}
					Local<Object> obj = data->objectTemplate->NewInstance();
					obj->SetPointerInInternalField(0, data->getUserData());
					setObjectSignature(&obj);
					return obj;
				}
				break;

			case mmitEnumValue:
				if(! userData->isInstance || userData->getParam()->getConfig().allowAccessEnumValueViaInstance()) {
					GScopedInterface<IMetaEnum> metaEnum(gdynamic_cast<IMetaEnum *>(mapItem->getItem()));
					return variantToV8(userData->getParam(), metaGetEnumValue(metaEnum, static_cast<uint32_t>(mapItem->getEnumIndex())), GMetaType(), true, false);
				}
				break;

			case mmitClass:
				if(! userData->isInstance || userData->getParam()->getConfig().allowAccessClassViaInstance()) {
					GScopedInterface<IMetaClass> innerMetaClass(gdynamic_cast<IMetaClass *>(mapItem->getItem()));
					Handle<FunctionTemplate> functionTemplate = createClassTemplate(userData->getParam(), name, innerMetaClass.get());
					return functionTemplate->GetFunction();
				}
				break;

			default:
				break;
		}
	}

	return Handle<Value>();
}

v8::Handle<v8::Value> setNamedMember(GClassUserData * userData, const char * name, v8::Local<v8::Value> value)
{
	GMetaClassTraveller traveller(userData->metaClass, userData->instance);

	void * instance = NULL;

	for(;;) {
		GScopedInterface<IMetaClass> metaClass(traveller.next(&instance));
		if(!metaClass) {
			break;
		}

		GMetaMapItem * mapItem = findMetaMapItem(userData->getParam()->getMetaMap(), metaClass.get(), name);
		if(mapItem == NULL) {
			continue;
		}

		bool error = false;

		switch(mapItem->getType()) {
			case mmitField:
			case mmitProperty: {
				GScopedInterface<IMetaAccessible> data(gdynamic_cast<IMetaAccessible *>(mapItem->getItem()));
				if(allowAccessData(userData, data.get())) {
					GVariant v = v8ToVariant(value).getValue();
					metaSetValue(data.get(), userData->instance, v);
					return value;
				}
			}
			   break;

			case mmitMethod:
			case mmitMethodList:
			case mmitEnum:
			case mmitEnumValue:
			case mmitClass:
				error = true;
				break;

			default:
				break;
		}

		if(error) {
			raiseCoreException(Error_ScriptBinding_CantAssignToEnumMethodClass);
			break;
		}
	}

	return Handle<Value>();
}

v8::Handle<v8::Value> staticMemberGetter(v8::Local<v8::String> prop, const v8::AccessorInfo & info)
{
	ENTER_V8()

	GClassUserData * userData = static_cast<GClassUserData *>(Local<External>::Cast(info.Data())->Value());

	String::Utf8Value utf8_prop(prop);
	const char * name = *utf8_prop;

	return getNamedMember(userData, name);

	LEAVE_V8(return Handle<Value>())
}

void staticMemberSetter(v8::Local<v8::String> prop, v8::Local<v8::Value> value, const v8::AccessorInfo & info)
{
	ENTER_V8()

	GClassUserData * userData = static_cast<GClassUserData *>(Local<External>::Cast(info.Data())->Value());

	String::Utf8Value utf8_prop(prop);
	const char * name = *utf8_prop;

	setNamedMember(userData, name, value);

	LEAVE_V8()
}

v8::Handle<v8::Value> namedMemberGetter(v8::Local<v8::String> prop, const v8::AccessorInfo & info)
{
	ENTER_V8()

	if(!isValidObject(info.Holder())) {
		raiseCoreException(Error_ScriptBinding_AccessMemberWithWrongObject);
	}

	String::Utf8Value utf8_prop(prop);
	const char * name = *utf8_prop;

	GClassUserData * userData = static_cast<GClassUserData *>(info.Holder()->GetPointerFromInternalField(0));

	return getNamedMember(userData, name);

	LEAVE_V8(return Handle<Value>())
}

v8::Handle<v8::Value> namedMemberSetter(v8::Local<v8::String> prop, v8::Local<v8::Value> value, const v8::AccessorInfo & info)
{
	ENTER_V8()

	String::Utf8Value utf8_prop(prop);
	const char * name = *utf8_prop;

	if(!isValidObject(info.Holder())) {
		raiseCoreException(Error_ScriptBinding_AccessMemberWithWrongObject);
	}

	GClassUserData * userData = static_cast<GClassUserData *>(info.Holder()->GetPointerFromInternalField(0));

	if(userData->cv == opcvConst) {
		raiseCoreException(Error_ScriptBinding_CantWriteToConstObject);

		return Handle<Value>();
	}

	if(userData == NULL) {
		return Handle<Value>();
	}

	return setNamedMember(userData, name, value);

	LEAVE_V8(return Handle<Value>())
}

void accessorNamedMemberSetter(v8::Local<v8::String> prop, v8::Local<v8::Value> value, const v8::AccessorInfo & info)
{
	namedEnumSetter(prop, value, info);
}

void bindClassItems(v8::Local<v8::Object> object, GScriptBindingParam * param, IMetaClass * metaClass, bool allowStatic, bool allowMember)
{
	GClassUserData * userData = new GClassUserData(param, metaClass, NULL, false, false, opcvNone);
	getUserDataPool()->addUserData(userData);
	Persistent<External> data = Persistent<External>::New(External::New(userData));
	data.MakeWeak(userData, weakHandleCallback);

	GScopedInterface<IMetaItem> item;
	uint32_t count = metaClass->getMetaCount();
	for(uint32_t i = 0; i < count; ++i) {
		item.reset(metaClass->getMetaAt(i));
		if(allowStatic && item->isStatic()) {
			object->SetAccessor(String::New(item->getName()), &staticMemberGetter, &staticMemberSetter, data);
		}
		else {
			if(allowMember && !item->isStatic()) {
				object->SetAccessor(String::New(item->getName()), &namedMemberGetter, &accessorNamedMemberSetter, data);
			}
		}
	}
}

void * invokeConstructor(const v8::Arguments & args, GScriptBindingParam * param, IMetaClass * metaClass)
{
	int paramCount = args.Length();
	void * instance = NULL;

	if(paramCount == 0 && metaClass->canCreateInstance()) {
		instance = metaClass->createInstance();
	}
	else {
		InvokeCallableParam callableParam;
		loadCallableParam(args, param, &callableParam);

		int maxRankIndex = findAppropriateCallable(param->getService(),
			makeCallback(metaClass, &IMetaClass::getConstructorAt), metaClass->getConstructorCount(),
			&callableParam, FindCallablePredict());

		if(maxRankIndex >= 0) {
			InvokeCallableResult result;

			GScopedInterface<IMetaConstructor> constructor(metaClass->getConstructorAt(static_cast<uint32_t>(maxRankIndex)));
			doInvokeCallable(NULL, constructor.get(), callableParam.paramsData, paramCount, &result);
			instance = fromVariant<void *>(GVariant(result.resultData));
		}
	}

	if(instance != NULL) {
		return instance;
	}
	else {
		raiseCoreException(Error_ScriptBinding_FailConstructObject);
	}

	return NULL;
}

v8::Handle<v8::Value> objectConstructor(const v8::Arguments & args)
{
	ENTER_V8()

	if(! args.IsConstructCall()) {
		return ThrowException(String::New("Cannot call constructor as function"));
	}

	Local<External> data = Local<External>::Cast(args.Data());
	GClassUserData * userData = static_cast<GClassUserData *>(data->Value());

	void * instance = invokeConstructor(args, userData->getParam(), userData->metaClass);
	Persistent<Object> self = Persistent<Object>::New(args.Holder());

	GClassUserData * instanceUserData = new GClassUserData(userData->getParam(), userData->metaClass, instance, true, true, opcvNone);
	getUserDataPool()->addUserData(instanceUserData);
	self.MakeWeak(instanceUserData, weakHandleCallback);

	self->SetPointerInInternalField(0, instanceUserData);
	setObjectSignature(&self);

	return self;

	LEAVE_V8(return Handle<Value>());
}

v8::Handle<v8::FunctionTemplate> createClassTemplate(GScriptBindingParam * param, const char * name, IMetaClass * metaClass)
{
	GMetaMapClass * map = param->getMetaMap()->findClassMap(metaClass);

	if(map->getData() != NULL) {
		return gdynamic_cast<GMapItemClassData *>(map->getData())->functionTemplate;
	}

	GClassUserData * userData = new GClassUserData(param, metaClass, NULL, false, false, opcvNone);
	getUserDataPool()->addUserData(userData);
	Persistent<External> data = Persistent<External>::New(External::New(userData));
	data.MakeWeak(userData, weakHandleCallback);

	Handle<FunctionTemplate> functionTemplate = FunctionTemplate::New(objectConstructor, data);
	functionTemplate->SetClassName(String::New(name));

	Local<ObjectTemplate> instanceTemplate = functionTemplate->InstanceTemplate();
	instanceTemplate->SetInternalFieldCount(1);

	instanceTemplate->SetNamedPropertyHandler(&namedMemberGetter, &namedMemberSetter);

	GMapItemClassData * mapData = new GMapItemClassData;
	mapData->setTemplate(functionTemplate);
	map->setData(mapData);

	if(metaClass->getBaseCount() > 0) {
		GScopedInterface<IMetaClass> baseClass(metaClass->getBaseClass(0));
		Handle<FunctionTemplate> baseFunctionTemplate = createClassTemplate(param, baseClass->getName(), baseClass.get());
		functionTemplate->Inherit(baseFunctionTemplate);
	}

	Local<Function> classFunction = functionTemplate->GetFunction();
	setObjectSignature(&classFunction);
	bindClassItems(classFunction, param, metaClass, true, false);

	GClassUserData * classUserData = new GClassUserData(param, metaClass, NULL, false, false, opcvNone);
	getUserDataPool()->addUserData(classUserData);
	Persistent<External> classData = Persistent<External>::New(External::New(classUserData));
	classData.MakeWeak(classUserData, weakHandleCallback);
	classFunction->SetHiddenValue(v8::String::New(userDataKey), classData);

	return functionTemplate;
}

void doBindClass(GScriptBindingParam * param, v8::Local<v8::Object> container, const char * name, IMetaClass * metaClass)
{
	Handle<FunctionTemplate> functionTemplate = createClassTemplate(param, name, metaClass);
	container->Set(v8::String::New(name), functionTemplate->GetFunction());
}

class GV8ScriptObjectImplement
{
public:
	GV8ScriptObjectImplement(IMetaService * service, v8::Local<v8::Object> object, const GScriptConfig & config, GMetaMap * metaMap, bool freeMap)
		: param(service, config, metaMap), object(v8::Persistent<v8::Object>::New(object)), freeMap(freeMap) {
	}

	~GV8ScriptObjectImplement() {
		if(this->freeMap) {
			delete this->param.getMetaMap();
		}

		this->object.Dispose();
	}

	void doBindMethodList(const char * name, IMetaList * methodList, GUserDataMethodType methodType)
	{
		v8::HandleScope handleScope;
		v8::Local<v8::Object> localObject(v8::Local<v8::Object>::New(this->object));

		GExtendMethodUserData * newUserData;
		Handle<FunctionTemplate> functionTemplate = createMethodTemplate(&this->param, NULL, true, methodList, name,
		Handle<FunctionTemplate>(), methodType, &newUserData);

		Persistent<Function> func = Persistent<Function>::New(functionTemplate->GetFunction());
		setObjectSignature(&func);
		func.MakeWeak(NULL, weakHandleCallback);

		localObject->Set(v8::String::New(name), func);
	}

	GExtendMethodUserData * doGetMethodUserData(const char * methodName)
	{
		v8::HandleScope handleScope;
		v8::Local<v8::Object> localObject(v8::Local<v8::Object>::New(this->object));

		Local<Value> value = localObject->Get(String::New(methodName));
		if(isValidObject(value)) {
			Local<Object> obj = Local<Object>::Cast(value);
			if(obj->InternalFieldCount() == 0) {
				Handle<Value> data = obj->GetHiddenValue(v8::String::New(userDataKey));
				if(! data.IsEmpty()) {
					if(data->IsExternal()) {
						GScriptUserData * userData = static_cast<GScriptUserData *>(Handle<External>::Cast(data)->Value());
						if(userData->getType() == udtExtendMethod) {
							GExtendMethodUserData * methodListData = gdynamic_cast<GExtendMethodUserData *>(userData);
							if(methodListData->methodList != NULL) {
								return methodListData;
							}
						}
					}
				}

			}
		}

		return NULL;
	}

public:
	GV8ScriptBindingParam param;
	v8::Persistent<v8::Object> object;
	bool freeMap;
};


GMAKE_FINAL(GV8ScriptObject)

class GV8ScriptObject : public GScriptObject, GFINAL_BASE(GV8ScriptObject)
{
private:
	typedef GScriptObject super;

public:
	GV8ScriptObject(IMetaService * service, v8::Local<v8::Object> object, const GScriptConfig & config);
	virtual ~GV8ScriptObject();

	virtual void bindClass(const char * name, IMetaClass * metaClass);
	virtual void bindEnum(const char * name, IMetaEnum * metaEnum);

	virtual void bindFundamental(const char * name, const GVariant & value);
	virtual void bindString(const char * stringName, const char * s);
	virtual void bindObject(const char * objectName, void * instance, IMetaClass * type, bool transferOwnership);
	virtual void bindRaw(const char * name, const GVariant & value);
	virtual void bindMethod(const char * name, void * instance, IMetaMethod * method);
	virtual void bindMethodList(const char * name, IMetaList * methodList);

	virtual IMetaClass * getClass(const char * className);
	virtual IMetaEnum * getEnum(const char * enumName);

	virtual GVariant getFundamental(const char * name);
	virtual std::string getString(const char * stringName);
	virtual void * getObject(const char * objectName);
	virtual GVariant getRaw(const char * name);
	virtual IMetaMethod * getMethod(const char * methodName, void ** outInstance);
	virtual IMetaList * getMethodList(const char * methodName);
	
	virtual GScriptDataType getType(const char * name, IMetaTypedItem ** outMetaTypeItem);

	virtual GScriptObject * createScriptObject(const char * name);
	virtual GScriptObject * gainScriptObject(const char * name);
	
	virtual GMetaVariant invoke(const char * name, const GMetaVariant * params, size_t paramCount);
	virtual GMetaVariant invokeIndirectly(const char * name, GMetaVariant const * const * params, size_t paramCount);

	virtual void assignValue(const char * fromName, const char * toName);
	virtual bool valueIsNull(const char * name);
	virtual void nullifyValue(const char * name);

	virtual void bindAccessible(const char * name, void * instance, IMetaAccessible * accessible);

private:
	GV8ScriptObject(const GV8ScriptObject & other, v8::Local<v8::Object> object);

private:
	GScopedPointer<GV8ScriptObjectImplement> implement;

private:
};


GV8ScriptObject::GV8ScriptObject(IMetaService * service, v8::Local<v8::Object> object, const GScriptConfig & config)
	: super(config)
{
	this->implement.reset(new GV8ScriptObjectImplement(service, object, config, new GMetaMap, true));
}

GV8ScriptObject::GV8ScriptObject(const GV8ScriptObject & other, v8::Local<v8::Object> object)
	: super(other.implement->param.getConfig())
{
	this->implement.reset(new GV8ScriptObjectImplement(other.implement->param.getService(), object, super::getConfig(), other.implement->param.getMetaMap(), false));
}

GV8ScriptObject::~GV8ScriptObject()
{
}

GScriptDataType GV8ScriptObject::getType(const char * name, IMetaTypedItem ** outMetaTypeItem)
{
	ENTER_V8()

	v8::HandleScope handleScope;
	v8::Local<v8::Object> localObject(v8::Local<v8::Object>::New(this->implement->object));

	Local<Value> obj = localObject->Get(String::New(name));
	return getV8Type(obj, outMetaTypeItem);

	LEAVE_V8(return sdtUnknown)
}

void GV8ScriptObject::bindClass(const char * name, IMetaClass * metaClass)
{
	ENTER_V8()

	v8::HandleScope handleScope;
	v8::Local<v8::Object> localObject(v8::Local<v8::Object>::New(this->implement->object));

	doBindClass(&this->implement->param, localObject, name, metaClass);

	LEAVE_V8()
}

void GV8ScriptObject::bindEnum(const char * name, IMetaEnum * metaEnum)
{
	ENTER_V8()

	v8::HandleScope handleScope;
	v8::Local<v8::Object> localObject(v8::Local<v8::Object>::New(this->implement->object));

	GEnumUserData * newUserData;
	Handle<ObjectTemplate> objectTemplate = createEnumTemplate(&this->implement->param, metaEnum, name, &newUserData);
	objectTemplate->SetInternalFieldCount(1);
	Persistent<Object> obj = Persistent<Object>::New(objectTemplate->NewInstance());
	obj->SetPointerInInternalField(0, newUserData);
	setObjectSignature(&obj);
	obj.MakeWeak(newUserData, weakHandleCallback);

	localObject->Set(String::New(name), obj);

	LEAVE_V8()
}

GScriptObject * GV8ScriptObject::createScriptObject(const char * name)
{
	ENTER_V8()

	v8::HandleScope handleScope;
	v8::Local<v8::Object> localObject(v8::Local<v8::Object>::New(this->implement->object));

	Local<Value> value = localObject->Get(String::New(name));
	if(isValidObject(value)) {
		return NULL;
	}

	Handle<ObjectTemplate> objectTemplate = ObjectTemplate::New();
	Local<Object> obj = objectTemplate->NewInstance();
	localObject->Set(String::New(name), obj);

	GV8ScriptObject * binding = new GV8ScriptObject(*this, obj);
	binding->owner = this;
	binding->name = name;

	return binding;

	LEAVE_V8(return NULL)
}

GScriptObject * GV8ScriptObject::gainScriptObject(const char * name)
{
	ENTER_V8()

	v8::HandleScope handleScope;
	v8::Local<v8::Object> localObject(v8::Local<v8::Object>::New(this->implement->object));

	Local<Value> value = localObject->Get(String::New(name));
	if((value->IsObject() || value->IsFunction()) && !isValidObject(value)) {
		GV8ScriptObject * binding = new GV8ScriptObject(*this, Local<Object>::Cast(value));
		binding->owner = this;
		binding->name = name;

		return binding;
	}
	else {
		return NULL;
	}

	LEAVE_V8(return NULL)
}

GMetaVariant GV8ScriptObject::invoke(const char * name, const GMetaVariant * params, size_t paramCount)
{
	GASSERT_MSG(paramCount <= REF_MAX_ARITY, "Too many parameters.");

	const cpgf::GMetaVariant * variantPointers[REF_MAX_ARITY];

	for(size_t i = 0; i < paramCount; ++i) {
		variantPointers[i] = &params[i];
	}

	return this->invokeIndirectly(name, variantPointers, paramCount);
}

GMetaVariant GV8ScriptObject::invokeIndirectly(const char * name, GMetaVariant const * const * params, size_t paramCount)
{
	GASSERT_MSG(paramCount <= REF_MAX_ARITY, "Too many parameters.");

	ENTER_V8()

	v8::HandleScope handleScope;
	v8::Local<v8::Object> localObject(v8::Local<v8::Object>::New(this->implement->object));

	Local<Value> func = localObject->Get(String::New(name));

	if(func->IsFunction() || (func->IsObject() && Local<Object>::Cast(func)->IsCallable())) {
		Handle<Value> v8Params[REF_MAX_ARITY];
		for(size_t i = 0; i < paramCount; ++i) {
			v8Params[i] = variantToV8(&this->implement->param, params[i]->getValue(), params[i]->getType(), false, true);
			if(v8Params[i].IsEmpty()) {
				raiseCoreException(Error_ScriptBinding_ScriptMethodParamMismatch, i, name);
			}
		}

		Local<Value> result;
		Handle<Object> receiver = localObject;

		if(func->IsFunction()) {
			result = Local<Function>::Cast(func)->Call(receiver, static_cast<int>(paramCount), v8Params);
		}
		else {
			result = Local<Object>::Cast(func)->CallAsFunction(receiver, static_cast<int>(paramCount), v8Params);
		}

		return v8ToVariant(result);
	}
	else {
		raiseCoreException(Error_ScriptBinding_CantCallNonfunction);
	}

	return GMetaVariant();

	LEAVE_V8(return GMetaVariant())
}

void GV8ScriptObject::bindFundamental(const char * name, const GVariant & value)
{
	GASSERT_MSG(vtIsFundamental(vtGetType(value.data.typeData)), "Only fundamental value can be bound via bindFundamental");
	
	ENTER_V8()

	v8::HandleScope handleScope;
	v8::Local<v8::Object> localObject(v8::Local<v8::Object>::New(this->implement->object));

	localObject->Set(v8::String::New(name), variantToV8(&this->implement->param, value, GMetaType(), false, true));

	LEAVE_V8()
}

void GV8ScriptObject::bindAccessible(const char * name, void * instance, IMetaAccessible * accessible)
{
	ENTER_V8()

	v8::HandleScope handleScope;
	v8::Local<v8::Object> localObject(v8::Local<v8::Object>::New(this->implement->object));

	doBindAccessible(&this->implement->param, localObject, name, instance, accessible);

	LEAVE_V8()
}

void GV8ScriptObject::bindString(const char * stringName, const char * s)
{
	ENTER_V8()

	v8::HandleScope handleScope;
	v8::Local<v8::Object> localObject(v8::Local<v8::Object>::New(this->implement->object));

	localObject->Set(v8::String::New(stringName), v8::String::New(s));

	LEAVE_V8()
}

void GV8ScriptObject::bindObject(const char * objectName, void * instance, IMetaClass * type, bool transferOwnership)
{
	ENTER_V8()

	v8::HandleScope handleScope;
	v8::Local<v8::Object> localObject(v8::Local<v8::Object>::New(this->implement->object));

	v8::Handle<v8::Value> obj = objectToV8(&this->implement->param, instance, type, transferOwnership, opcvNone);
	localObject->Set(v8::String::New(objectName), obj);

	LEAVE_V8()
}

void GV8ScriptObject::bindRaw(const char * name, const GVariant & value)
{
	GASSERT_MSG(vtIsFundamental(vtGetType(value.data.typeData)), "Only fundamental value can be bound via bindFundamental");
	
	ENTER_V8()

	v8::HandleScope handleScope;
	v8::Local<v8::Object> localObject(v8::Local<v8::Object>::New(this->implement->object));

	localObject->Set(v8::String::New(name), rawToV8(&this->implement->param, value));

	LEAVE_V8()
}

void GV8ScriptObject::bindMethod(const char * name, void * instance, IMetaMethod * method)
{
	ENTER_V8()

	if(method->isStatic()) {
		instance = NULL;
	}

	GScopedInterface<IMetaList> methodList(createMetaList());
	methodList->add(method, instance);

	this->implement->doBindMethodList(name, methodList.get(), udmtMethod);

	LEAVE_V8()
}

void GV8ScriptObject::bindMethodList(const char * name, IMetaList * methodList)
{
	ENTER_V8()

	this->implement->doBindMethodList(name, methodList, udmtMethodList);

	LEAVE_V8()
}

IMetaClass * GV8ScriptObject::getClass(const char * className)
{
	IMetaTypedItem * typedItem = NULL;

	GScriptDataType sdt = this->getType(className, &typedItem);
	GScopedInterface<IMetaTypedItem> item(typedItem);
	if(sdt == sdtClass) {
		return gdynamic_cast<IMetaClass *>(item.take());
	}

	return NULL;
}

IMetaEnum * GV8ScriptObject::getEnum(const char * enumName)
{
	IMetaTypedItem * typedItem = NULL;

	GScriptDataType sdt = this->getType(enumName, &typedItem);
	GScopedInterface<IMetaTypedItem> item(typedItem);
	if(sdt == sdtEnum) {
		return gdynamic_cast<IMetaEnum *>(item.take());
	}

	return NULL;
}

GVariant GV8ScriptObject::getFundamental(const char * name)
{
	ENTER_V8()

	v8::HandleScope handleScope;
	v8::Local<v8::Object> localObject(v8::Local<v8::Object>::New(this->implement->object));

	Local<Value> value = localObject->Get(String::New(name));
	if(getV8Type(value, NULL) == sdtFundamental) {
		return v8ToVariant(value).getValue();
	}
	else {
		return GVariant();
	}

	LEAVE_V8(return GVariant())
}

std::string GV8ScriptObject::getString(const char * stringName)
{
	ENTER_V8()

	v8::HandleScope handleScope;
	v8::Local<v8::Object> localObject(v8::Local<v8::Object>::New(this->implement->object));

	Local<Value> value = localObject->Get(String::New(stringName));
	if(value->IsString()) {
		v8::String::AsciiValue s(value);
		return *s;
	}
	else {
		return "";
	}

	LEAVE_V8(return "")
}

void * GV8ScriptObject::getObject(const char * objectName)
{
	ENTER_V8()

	v8::HandleScope handleScope;
	v8::Local<v8::Object> localObject(v8::Local<v8::Object>::New(this->implement->object));

	Local<Value> value = localObject->Get(String::New(objectName));
	return v8ToObject(value, NULL);

	LEAVE_V8(return NULL)
}

GVariant GV8ScriptObject::getRaw(const char * name)
{
	ENTER_V8()

	v8::HandleScope handleScope;
	v8::Local<v8::Object> localObject(v8::Local<v8::Object>::New(this->implement->object));

	Local<Value> value = localObject->Get(String::New(name));
	if(getV8Type(value, NULL) == sdtRaw) {
		return v8ToVariant(value).getValue();
	}
	else {
		return GVariant();
	}

	LEAVE_V8(return GVariant())
}

IMetaMethod * GV8ScriptObject::getMethod(const char * methodName, void ** outInstance)
{
	ENTER_V8()

	if(outInstance != NULL) {
		*outInstance = NULL;
	}

	GExtendMethodUserData * userData = this->implement->doGetMethodUserData(methodName);
	if(userData != NULL && userData->methodType == udmtMethod) {
		if(outInstance != NULL) {
			*outInstance = userData->methodList->getInstanceAt(0);
		}

		return gdynamic_cast<IMetaMethod *>(userData->methodList->getAt(0));
	}
	else {
		return NULL;
	}

	LEAVE_V8(return NULL)
}

IMetaList * GV8ScriptObject::getMethodList(const char * methodName)
{
	ENTER_V8()

	GExtendMethodUserData * userData = this->implement->doGetMethodUserData(methodName);
	if(userData != NULL && userData->methodType == udmtMethodList) {
		userData->methodList->addReference();

		return userData->methodList;
	}
	else {
		return NULL;
	}

	LEAVE_V8(return NULL)
}

void GV8ScriptObject::assignValue(const char * fromName, const char * toName)
{
	ENTER_V8()

	v8::HandleScope handleScope;
	v8::Local<v8::Object> localObject(v8::Local<v8::Object>::New(this->implement->object));

	Local<Value> value = localObject->Get(String::New(fromName));
	localObject->Set(String::New(toName), value);

	LEAVE_V8()
}

bool GV8ScriptObject::valueIsNull(const char * name)
{
	ENTER_V8()

	v8::HandleScope handleScope;
	v8::Local<v8::Object> localObject(v8::Local<v8::Object>::New(this->implement->object));

	Local<Value> value = localObject->Get(String::New(name));
	return value.IsEmpty() || value->IsUndefined() || value->IsNull();

	LEAVE_V8(return false)
}

void GV8ScriptObject::nullifyValue(const char * name)
{
	ENTER_V8()

	HandleScope handleScope;
	Local<Object> localObject(Local<Object>::New(this->implement->object));

	localObject->Set(String::New(name), v8::Null());

	LEAVE_V8()
}


} // unnamed namespace


GScriptObject * createV8ScriptObject(IMetaService * service, v8::Local<v8::Object> object, const GScriptConfig & config)
{
	return new GV8ScriptObject(service, object, config);
}

IScriptObject * createV8ScriptInterface(IMetaService * service, v8::Local<v8::Object> object, const GScriptConfig & config)
{
	return new ImplScriptObject(new GV8ScriptObject(service, object, config));
}



} // namespace cpgf




#if defined(_MSC_VER)
#pragma warning(pop)
#endif

