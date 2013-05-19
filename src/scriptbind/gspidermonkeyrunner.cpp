#include "cpgf/scriptbind/gscriptrunner.h"
#include "cpgf/private/gscriptrunner_p.h"
#include "cpgf/scriptbind/gscriptbind.h"
#include "cpgf/scriptbind/gspidermonkeybind.h"
#include "cpgf/gmetaapi.h"

#include <stdexcept>


namespace cpgf {


namespace {

class GSpiderMonkeyScriptRunnerImplement : public GScriptRunnerImplement
{
private:
	typedef GScriptRunnerImplement super;

public:
	GSpiderMonkeyScriptRunnerImplement(IMetaService * service);
	~GSpiderMonkeyScriptRunnerImplement();

	virtual void executeString(const char * code);

private:
	bool executeJsString(const char * source);
	void error(const char * message) const;

private:
	JSRuntime * jsRuntime;
	JSContext * jsContext;
	JSObject  * jsGlobal;
};


void reportError(JSContext * /*jsContext*/, const char * message, JSErrorReport * /*report*/)
{
	fprintf(stderr, "SpiderMonkey error: %s\n", message);
}

GSpiderMonkeyScriptRunnerImplement::GSpiderMonkeyScriptRunnerImplement(IMetaService * service)
	: super(service), jsRuntime(JS_NewRuntime(128L * 1024L * 1024L, JS_NO_HELPER_THREADS))
{
	this->jsContext = JS_NewContext(this->jsRuntime, 8192);
	JS_SetOptions(this->jsContext, JSOPTION_METHODJIT);
	JS_SetVersion(this->jsContext, JSVERSION_LATEST);
	JS_SetErrorReporter(this->jsContext, &reportError);
	this->jsGlobal = createSpiderMonkeyGlobaObject(this->jsContext);
	JS_InitStandardClasses(this->jsContext, this->jsGlobal);

	GScopedInterface<IScriptObject> scriptObject(createSpiderMonkeyScriptInterface(this->getService(), this->jsContext, this->jsGlobal, GScriptConfig()));
	this->setScripeObject(scriptObject.get());
}

GSpiderMonkeyScriptRunnerImplement::~GSpiderMonkeyScriptRunnerImplement()
{
	JS_ClearNonGlobalObject(this->jsContext, this->jsGlobal);
	
	JS_DestroyContext(this->jsContext);
	JS_DestroyRuntime(this->jsRuntime);
	JS_ShutDown();
}

bool GSpiderMonkeyScriptRunnerImplement::executeJsString(const char * source)
{
	jsval result;
	JSBool success = JS_EvaluateScript(this->jsContext, this->jsGlobal, source, (unsigned int)strlen(source), "script", 1, &result);
	return success == JS_TRUE;
}

void GSpiderMonkeyScriptRunnerImplement::executeString(const char * code)
{
	this->executeJsString(code);
}

void GSpiderMonkeyScriptRunnerImplement::error(const char * message) const
{
	throw std::runtime_error(message);
}


} // unnamed namespace


GScriptRunner * createSpiderMonkeyScriptRunner(IMetaService * service)
{
	return GScriptRunnerImplement::createScriptRunner(new GSpiderMonkeyScriptRunnerImplement(service));
}



} // namespace cpgf
