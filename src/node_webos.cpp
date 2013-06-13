/* @@@LICENSE
*
*      Copyright (c) 2010-2013 LG Electronics, Inc.
*
* Licensed under the Apache License, Version 2.0 (the "License");
* you may not use this file except in compliance with the License.
* You may obtain a copy of the License at
*
* http://www.apache.org/licenses/LICENSE-2.0
*
* Unless required by applicable law or agreed to in writing, software
* distributed under the License is distributed on an "AS IS" BASIS,
* WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
* See the License for the specific language governing permissions and
* limitations under the License.
*
* LICENSE@@@ */

#include <boost/filesystem.hpp>
#include <node.h>
#include <v8.h>

#include "external_string.h"

namespace bf = boost::filesystem;

using namespace v8;
using namespace std;

const char* kFileNameGlobal="__filename";
const char* kDirNameGlobal="__dirname";

static void SetFileAndDirectoryGlobals(Local<Object> global, const char* path)
{
	bf::path pathToFile(bf::system_complete(bf::path(path)));
	bf::path pathToParentDir(pathToFile.parent_path());
	Handle<String> fileName = v8::String::New(pathToFile.string().c_str());
	global->Set(String::NewSymbol(kFileNameGlobal), fileName);
	Handle<String> dirName = v8::String::New(pathToParentDir.string().c_str());
	global->Set(String::NewSymbol(kDirNameGlobal), dirName);
}

static void ClearFileAndDirectoryGlobals(Local<Object> global)
{
	global->Set(String::NewSymbol(kFileNameGlobal), v8::Undefined());
	global->Set(String::NewSymbol(kDirNameGlobal), v8::Undefined());
}

// Load, compile and execute a JavaScript file in the current context. Used by
// the webOS unit test framework and service launcher, as well as part of the implementation
// of the webOS custom require function below.
Handle<Value> IncludeScript(char const * pathToScriptSource, bool& exceptionOccurred)
{
	exceptionOccurred = true;
	if(!pathToScriptSource || !*pathToScriptSource ) {
        return ThrowException(Exception::Error(
                                  String::New("webOS 'include' requires a non-empty filename argument.")));
	}
	HandleScope scope;
	Handle<Value> returnValue = Undefined();
	Local<String> scriptSource = createV8StringFromFile(pathToScriptSource);
	Handle<Script> compiledScript(Script::Compile(scriptSource, String::New(pathToScriptSource)));
	if(compiledScript.IsEmpty()) {
		return returnValue;
	}
	Local<Context> currentContext = Context::GetCurrent();
	Local<Object> global = currentContext->Global();
	SetFileAndDirectoryGlobals(global, pathToScriptSource);
	returnValue = compiledScript->Run();
	ClearFileAndDirectoryGlobals(global);
	if(returnValue.IsEmpty()) {
		return returnValue;
	}
	exceptionOccurred = false;            
	return scope.Close(returnValue);
}

// Wrapper function that checks and converts parameters on the way in and converts
// exceptions.
Handle<Value> IncludeScriptWrapper( Arguments const & arguments )
{
    if (arguments.Length() != 1) {
        return ThrowException(Exception::Error(
                                  String::New("Invalid number of parameters, 1 expected.")));
    }
    try {
		v8::String::Utf8Value fileName(arguments[0]);
		bool exceptionOccurred;
		return IncludeScript(*fileName, exceptionOccurred);
    } catch( std::exception const & ex ) {
        return v8::ThrowException( v8::Exception::Error(v8::String::New(ex.what())));
    } catch( ... ) {
        return v8::ThrowException( v8::Exception::Error(v8::String::New("Native function threw an unknown exception.")));
    }
}

static void CopyProperty(const Handle<Object>& src, const Handle<Object>& dst, const char* propertyName)
{
	Local<String> pName(String::NewSymbol(propertyName));
	Local<Value> v = src->Get(pName);
	dst->Set(pName, v);
}

// Function that creates a new JavaScript context and loads, compiles and executes a list of source
// files in that context. Compatible with the CommonJS module specification.
// This implementation is imperfect, though, as it can't import all the apparently global symbols
// required from node. In particular, the node require() function is in fact a local variable, and not
// possible to access from this function. At this point this function is only an interesting experiment.
static Handle<Value> Require(const Handle<Value>& nativeRequire, const Handle<Value>& loader, const Handle<Array> & filePaths)
{
	// fetch the current content and global object
	Local<Context> currentContext = Context::GetCurrent();
	Local<Object> currentGlobal = currentContext->Global();
	
	// create a new context with an empty global template. This would be the place we'd
	// extend the global template with the function from node if that were possible.
	Handle<ObjectTemplate> globalTemplate = ObjectTemplate::New();
	Persistent<Context> utilityContext = Context::New(NULL, globalTemplate);
	
	// If security tokens don't match between contexts then neither context can access each
	// other's properties. This is the mechanism that keeps JS in pages in a browser from sniffing
	// other pages data. It's not being used for any purpose in webOS's use of node.
	utilityContext->SetSecurityToken(currentContext->GetSecurityToken());
	Context::Scope utilityScope(utilityContext);
	
	// Set up an exports object for use by modules.
	Handle<ObjectTemplate> exportsTemplate = ObjectTemplate::New();
	Local<Object> exportsInstance = exportsTemplate->NewInstance();
	Local<Object> global = utilityContext->Global();
	global->Set(String::NewSymbol("exports"), exportsInstance);
	global->Set(String::NewSymbol("global"), global);
	global->Set(String::NewSymbol("globals"), currentGlobal);
	global->Set(String::NewSymbol("root"), currentGlobal);
	global->Set(String::NewSymbol("MojoLoader"), loader);
	global->Set(String::NewSymbol("require"), nativeRequire);
	
	// copy a number of useful properties from the loading node context.
	CopyProperty(currentGlobal, global, "console");
	CopyProperty(currentGlobal, global, "setTimeout");
	CopyProperty(currentGlobal, global, "clearTimeout");
	CopyProperty(currentGlobal, global, "setInterval");
	CopyProperty(currentGlobal, global, "clearInterval");
	
	// load the list of files, stopping if any produce an error
	uint32_t length = filePaths->Length();
	for(uint32_t i = 0; i < length; ++i) {
		Local<Value> fileNameObject(filePaths->Get(i));
		if (!fileNameObject->IsString()) {
	        return ThrowException(Exception::Error(
	                                  String::New("All elements of file paths array must be strings.")));
		}
		v8::String::Utf8Value fileName(fileNameObject);
		bool exceptionOccurred;
		SetFileAndDirectoryGlobals(global, *fileName);
		IncludeScript(*fileName, exceptionOccurred);
		if (exceptionOccurred) {
			break;
		}
	}
	ClearFileAndDirectoryGlobals(global);
	return global;
}

static Handle<Value> RequireWrapper(const Arguments& arguments)
{
    if (arguments.Length() != 3) {
        return ThrowException(Exception::Error(
                                  String::New("Invalid number of parameters, 3 expected.")));
    }
	if (!arguments[0]->IsFunction()) {
        return ThrowException(Exception::Error(
                                  String::New("Argument 2 must be an function.")));		
	}
	if (!arguments[2]->IsArray()) {
        return ThrowException(Exception::Error(
                                  String::New("Argument 3 must be an array.")));		
	}
	Local<Array> fileList = Local<Array>::Cast(arguments[2]);
	return Require(arguments[0], arguments[1], fileList);
}


init(Handle<Object> target)
{
    HandleScope scope;
    Local<FunctionTemplate> includeTemplate = FunctionTemplate::New(IncludeScriptWrapper);
    target->Set(String::NewSymbol("include"), includeTemplate->GetFunction());
    Local<FunctionTemplate> requireTemplate = FunctionTemplate::New(RequireWrapper);
    target->Set(String::NewSymbol("require"), requireTemplate->GetFunction());
}

NODE_MODULE(webos, init)
