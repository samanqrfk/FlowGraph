
#pragma once

#include "Modules/ModuleInterface.h"

class UFlowAsset;

#if WITH_EDITOR
class IFlowEditorModuleInterface : public IModuleInterface
{
public:
	virtual void DeserializeEdGraphFromJSON(UFlowAsset* FlowAsset, TSharedPtr<FJsonObject> Json) = 0;
};
#endif
