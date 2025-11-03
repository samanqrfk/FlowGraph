
#pragma once

#include "Modules/ModuleInterface.h"

enum class EFlowAssetJSONSerializationMode : uint8;
class UFlowAsset;

#if WITH_EDITOR
class IFlowEditorModuleInterface : public IModuleInterface
{
public:
	virtual void DeserializeEdGraphFromJSON(UFlowAsset* FlowAsset, TSharedPtr<FJsonObject> Json, EFlowAssetJSONSerializationMode Mode) = 0;
};
#endif
