// NDCBinderUncookedModule.cpp
//
// Nothing to do on startup: the module exists to be the home of a K2Node class, which the Blueprint
// action database finds by reflection. See NDCBinderUncooked.Build.cs for why that home cannot be the
// editor module.

#include "Modules/ModuleManager.h"

IMPLEMENT_MODULE(FDefaultModuleImpl, NDCBinderUncooked);
