// Copyright https://github.com/MothCocoon/FlowGraph/graphs/contributors

#include "FlowAsset_Runtime.h"
#include "FlowLogChannels.h"
#include "Nodes/FlowNode.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(FlowAsset_Runtime)

//////////////////////////////////////////////////////////////////////////
// FFlowAssetRuntimeBuilder

FFlowAssetRuntimeBuilder::FFlowAssetRuntimeBuilder(UFlowAsset* InAsset)
	: Asset(InAsset)
{
	if (!Asset)
	{
		UE_LOG(LogFlow, Error, TEXT("FFlowAssetRuntimeBuilder: Asset is null"));
	}
}

FFlowAssetRuntimeBuilder& FFlowAssetRuntimeBuilder::AddNode(TSubclassOf<UFlowNode> NodeClass, FGuid& OutNodeGuid)
{
	if (!Asset)
	{
		UE_LOG(LogFlow, Error, TEXT("FFlowAssetRuntimeBuilder::AddNode: Asset is null"));
		OutNodeGuid = FGuid();
		return *this;
	}
	
	// Generate a new GUID
	OutNodeGuid = FGuid::NewGuid();
	
	UFlowNode* CreatedNode = Asset->CreateNodeAtRuntime(NodeClass, OutNodeGuid);
	if (!CreatedNode)
	{
		UE_LOG(LogFlow, Error, TEXT("FFlowAssetRuntimeBuilder::AddNode: Failed to create node of class %s"), 
			*NodeClass->GetName());
		OutNodeGuid = FGuid();
	}
	
	return *this;
}

FFlowAssetRuntimeBuilder& FFlowAssetRuntimeBuilder::AddNodeWithGuid(TSubclassOf<UFlowNode> NodeClass, const FGuid& NodeGuid)
{
	if (!Asset)
	{
		UE_LOG(LogFlow, Error, TEXT("FFlowAssetRuntimeBuilder::AddNodeWithGuid: Asset is null"));
		return *this;
	}
	
	UFlowNode* CreatedNode = Asset->CreateNodeAtRuntime(NodeClass, NodeGuid);
	if (!CreatedNode)
	{
		UE_LOG(LogFlow, Error, TEXT("FFlowAssetRuntimeBuilder::AddNodeWithGuid: Failed to create node of class %s"), 
			*NodeClass->GetName());
	}
	
	return *this;
}

FFlowAssetRuntimeBuilder& FFlowAssetRuntimeBuilder::Connect(const FGuid& FromNode, FName FromPin, const FGuid& ToNode, FName ToPin)
{
	if (!Asset)
	{
		UE_LOG(LogFlow, Error, TEXT("FFlowAssetRuntimeBuilder::Connect: Asset is null"));
		return *this;
	}
	
	if (!Asset->ConnectNodesAtRuntime(FromNode, FromPin, ToNode, ToPin))
	{
		UE_LOG(LogFlow, Warning, TEXT("FFlowAssetRuntimeBuilder::Connect: Failed to connect nodes"));
	}
	
	return *this;
}

FFlowAssetRuntimeBuilder& FFlowAssetRuntimeBuilder::ConnectDefault(const FGuid& FromNode, const FGuid& ToNode)
{
	// Use default pin names "Out" and "In"
	return Connect(FromNode, FName(TEXT("Out")), ToNode, FName(TEXT("In")));
}

UFlowNode* FFlowAssetRuntimeBuilder::GetNode(const FGuid& NodeGuid) const
{
	if (!Asset)
	{
		UE_LOG(LogFlow, Error, TEXT("FFlowAssetRuntimeBuilder::GetNode: Asset is null"));
		return nullptr;
	}
	
	return Asset->GetNode(NodeGuid);
}

UFlowAsset* FFlowAssetRuntimeBuilder::Build()
{
	if (!Asset)
	{
		UE_LOG(LogFlow, Error, TEXT("FFlowAssetRuntimeBuilder::Build: Asset is null"));
		return nullptr;
	}
	
	UE_LOG(LogFlow, Log, TEXT("FFlowAssetRuntimeBuilder::Build: Completed building runtime FlowAsset with %d nodes"), 
		Asset->GetNodes().Num());
	
	return Asset;
}

//////////////////////////////////////////////////////////////////////////
// UFlowAsset_Runtime

UFlowAsset_Runtime::UFlowAsset_Runtime(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	// Mark as runtime-generated from the start
	bIsRuntimeGenerated = true;
}

FFlowAssetRuntimeBuilder UFlowAsset_Runtime::CreateBuilder()
{
	return FFlowAssetRuntimeBuilder(this);
}

UFlowAsset_Runtime* UFlowAsset_Runtime::CreateRuntimeFlowAsset(UObject* WorldContextObject, FName AssetName)
{
	if (!WorldContextObject)
	{
		UE_LOG(LogFlow, Error, TEXT("UFlowAsset_Runtime::CreateRuntimeFlowAsset: WorldContextObject is null"));
		return nullptr;
	}
	
	// If no name provided, generate one
	if (AssetName == NAME_None)
	{
		AssetName = MakeUniqueObjectName(WorldContextObject, UFlowAsset_Runtime::StaticClass(), TEXT("RuntimeFlowAsset"));
	}
	
	UFlowAsset_Runtime* NewAsset = NewObject<UFlowAsset_Runtime>(WorldContextObject, AssetName, RF_Transient);
	
	if (NewAsset)
	{
		UE_LOG(LogFlow, Verbose, TEXT("UFlowAsset_Runtime::CreateRuntimeFlowAsset: Created runtime FlowAsset '%s'"), 
			*AssetName.ToString());
	}
	else
	{
		UE_LOG(LogFlow, Error, TEXT("UFlowAsset_Runtime::CreateRuntimeFlowAsset: Failed to create runtime FlowAsset"));
	}
	
	return NewAsset;
}
