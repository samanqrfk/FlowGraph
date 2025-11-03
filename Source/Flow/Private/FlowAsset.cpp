// Copyright https://github.com/MothCocoon/FlowGraph/graphs/contributors

#include "FlowAsset.h"

#include "FlowLogChannels.h"
#include "FlowSettings.h"
#include "FlowSubsystem.h"

#include "AddOns/FlowNodeAddOn.h"
#include "Nodes/FlowNodeBase.h"
#include "Nodes/FlowNode.h"
#include "Nodes/Graph/FlowNode_CustomInput.h"
#include "Nodes/Graph/FlowNode_CustomOutput.h"
#include "Nodes/Graph/FlowNode_Start.h"
#include "Nodes/Graph/FlowNode_SubGraph.h"

#include "Engine/World.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"

#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "JsonObjectConverter.h"
#include "FlowEditorModuleInterface.h"

#include "Serialization/JsonReader.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonSerializer.h"

#if WITH_EDITOR
	#include "Editor.h"
	#include "Editor/EditorEngine.h"

FString UFlowAsset::ValidationError_NodeClassNotAllowed = TEXT("Node class {0} is not allowed in this asset.");
FString UFlowAsset::ValidationError_NullNodeInstance = TEXT("Node with GUID {0} is NULL");

namespace FlowHarvestHelpers
{
	bool AreConnectedPinMapsEqual(const TMap<FName, FConnectedPin>& MapA, const TMap<FName, FConnectedPin>& MapB)
	{
		if (MapA.Num() != MapB.Num())
		{
			return false;
		}
		for (const auto& PairA : MapA)
		{
			const FConnectedPin* ValueB = MapB.Find(PairA.Key);
			if (!ValueB || PairA.Value != *ValueB)
			{
				return false;
			}
		}
		return true;
	}

	// Helper to compare FPinConnectionList (ignoring order in the array)
	bool ArePinConnectionListsEqual(const FPinConnectionList* ListA, const FPinConnectionList* ListB)
	{
		if (!ListA && !ListB)
		{
			return true;
		}
		if (!ListA || !ListB)
		{
			return false;
		}
		if (ListA->Connections.Num() != ListB->Connections.Num())
		{
			return false;
		}

		for (const FConnectedPin& ConnA : ListA->Connections)
		{
			bool bFoundMatch = false;
			for (const FConnectedPin& ConnB : ListB->Connections)
			{
				if (ConnA == ConnB)
				{
					bFoundMatch = true;
					break;
				}
			}
			if (!bFoundMatch)
			{
				return false;
			}
		}
		return true;
	}
} // namespace FlowHarvestHelpers
#endif

#include UE_INLINE_GENERATED_CPP_BY_NAME(FlowAsset)

UFlowAsset::UFlowAsset(const FObjectInitializer& ObjectInitializer)
    : Super(ObjectInitializer), bWorldBound(true)
#if WITH_EDITORONLY_DATA
      ,
      FlowGraph(nullptr)
#endif
      ,
      AllowedNodeClasses({UFlowNodeBase::StaticClass()}), AllowedInSubgraphNodeClasses({UFlowNode_SubGraph::StaticClass()}), bStartNodePlacedAsGhostNode(false), TemplateAsset(nullptr), FinishPolicy(EFlowFinishPolicy::Keep)
{
	if (!AssetGuid.IsValid())
	{
		AssetGuid = FGuid::NewGuid();
	}

	ExpectedOwnerClass = UFlowSettings::Get()->GetDefaultExpectedOwnerClass();
}

#if WITH_EDITOR
void UFlowAsset::AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector)
{
	UFlowAsset* This = CastChecked<UFlowAsset>(InThis);
	Collector.AddReferencedObject(This->FlowGraph, This);

	Super::AddReferencedObjects(InThis, Collector);
}

void UFlowAsset::PostEditChangeProperty(FPropertyChangedEvent& PropertyChangedEvent)
{
	Super::PostEditChangeProperty(PropertyChangedEvent);

	if (PropertyChangedEvent.Property && (PropertyChangedEvent.GetPropertyName() == GET_MEMBER_NAME_CHECKED(UFlowAsset, CustomInputs) || PropertyChangedEvent.GetPropertyName() == GET_MEMBER_NAME_CHECKED(UFlowAsset, CustomOutputs)))
	{
		OnSubGraphReconstructionRequested.Broadcast();
	}
}

void UFlowAsset::PostDuplicate(bool bDuplicateForPIE)
{
	Super::PostDuplicate(bDuplicateForPIE);

	if (!bDuplicateForPIE)
	{
		AssetGuid = FGuid::NewGuid();
		Nodes.Empty();
	}
}

void UFlowAsset::PostLoad()
{
	Super::PostLoad();

	// If we removed or moved a flow node blueprint (and there is no redirector) we might loose the reference to it resulting
	// in null pointers in the Nodes FGUID->UFlowNode* Map. So here we iterate over all the Nodes and remove all pairs that
	// are nulled out.

	TSet<FGuid> NodesToRemoveGUID;

	for (auto& [Guid, Node] : GetNodes())
	{
		if (!IsValid(Node))
		{
			NodesToRemoveGUID.Emplace(Guid);
		}
	}

	for (const FGuid& Guid : NodesToRemoveGUID)
	{
		UnregisterNode(Guid);
	}
}

EDataValidationResult UFlowAsset::ValidateAsset(FFlowMessageLog& MessageLog)
{
	// validate nodes
	for (const TPair<FGuid, UFlowNode*>& Node : ObjectPtrDecay(Nodes))
	{
		if (IsValid(Node.Value))
		{
			FText FailureReason;
			if (!IsNodeOrAddOnClassAllowed(Node.Value->GetClass(), &FailureReason))
			{
				const FString ErrorMsg =
				    FailureReason.IsEmpty()
				    ? FString::Format(*ValidationError_NodeClassNotAllowed, {*Node.Value->GetClass()->GetName()})
				    : FailureReason.ToString();

				MessageLog.Error(*ErrorMsg, Node.Value);
			}

			Node.Value->ValidationLog.Messages.Empty();
			if (Node.Value->ValidateNode() == EDataValidationResult::Invalid)
			{
				MessageLog.Messages.Append(Node.Value->ValidationLog.Messages);
			}
		}
		else
		{
			const FString ErrorMsg = FString::Format(*ValidationError_NullNodeInstance, {*Node.Key.ToString()});
			MessageLog.Error(*ErrorMsg, this);
		}
	}

	return MessageLog.Messages.Num() > 0 ? EDataValidationResult::Invalid : EDataValidationResult::Valid;
}

bool UFlowAsset::IsNodeOrAddOnClassAllowed(const UClass* FlowNodeOrAddOnClass, FText* OutOptionalFailureReason) const
{
	if (!IsValid(FlowNodeOrAddOnClass))
	{
		return false;
	}

	if (!CanFlowNodeClassBeUsedByFlowAsset(*FlowNodeOrAddOnClass))
	{
		return false;
	}

	if (!CanFlowAssetUseFlowNodeClass(*FlowNodeOrAddOnClass))
	{
		return false;
	}

	// Confirm plugin reference restrictions are being respected
	if (!CanFlowAssetReferenceFlowNode(*FlowNodeOrAddOnClass, OutOptionalFailureReason))
	{
		return false;
	}

	return true;
}

bool UFlowAsset::CanFlowNodeClassBeUsedByFlowAsset(const UClass& FlowNodeClass) const
{
	UFlowNode* NodeDefaults = Cast<UFlowNode>(FlowNodeClass.GetDefaultObject());
	if (!NodeDefaults)
	{
		check(FlowNodeClass.IsChildOf<UFlowNodeAddOn>());

		// AddOns don't have the AllowedAssetClasses/DeniedAssetClasses
		// (yet?  maybe we move it up to the base?)
		return true;
	}

	// UFlowNode class limits which UFlowAsset class can use it
	const TArray<TSubclassOf<UFlowAsset>>& DeniedAssetClasses = NodeDefaults->DeniedAssetClasses;
	for (const UClass* DeniedAssetClass : DeniedAssetClasses)
	{
		if (DeniedAssetClass && GetClass()->IsChildOf(DeniedAssetClass))
		{
			return false;
		}
	}

	const TArray<TSubclassOf<UFlowAsset>>& AllowedAssetClasses = NodeDefaults->AllowedAssetClasses;
	if (AllowedAssetClasses.Num() > 0)
	{
		bool bAllowedInAsset = false;
		for (const UClass* AllowedAssetClass : AllowedAssetClasses)
		{
			if (AllowedAssetClass && GetClass()->IsChildOf(AllowedAssetClass))
			{
				bAllowedInAsset = true;
				break;
			}
		}
		if (!bAllowedInAsset)
		{
			return false;
		}
	}

	return true;
}

bool UFlowAsset::CanFlowAssetUseFlowNodeClass(const UClass& FlowNodeClass) const
{
	// UFlowAsset class can limit which UFlowNodeBase classes can be used
	if (IsFlowNodeClassInDeniedClasses(FlowNodeClass))
	{
		return false;
	}

	if (!IsFlowNodeClassInAllowedClasses(FlowNodeClass))
	{
		return false;
	}

	return true;
}

bool UFlowAsset::IsFlowNodeClassInDeniedClasses(const UClass& FlowNodeClass) const
{
	for (const TSubclassOf<UFlowNodeBase>& DeniedNodeClass : DeniedNodeClasses)
	{
		if (DeniedNodeClass && FlowNodeClass.IsChildOf(DeniedNodeClass))
		{
			// Subclasses of a DeniedNodeClass can opt back in to being allowed
			if (!IsFlowNodeClassInAllowedClasses(FlowNodeClass, DeniedNodeClass))
			{
				return true;
			}
		}
	}

	return false;
}

bool UFlowAsset::IsFlowNodeClassInAllowedClasses(const UClass& FlowNodeClass,
    const TSubclassOf<UFlowNodeBase>& RequiredAncestor) const
{
	if (AllowedNodeClasses.Num() > 0)
	{
		bool bAllowedInAsset = false;
		for (const TSubclassOf<UFlowNodeBase>& AllowedNodeClass : AllowedNodeClasses)
		{
			// If a RequiredAncestor is provided, the AllowedNodeClass must be a subclass of the RequiredAncestor
			if (AllowedNodeClass && FlowNodeClass.IsChildOf(AllowedNodeClass) && (!RequiredAncestor || AllowedNodeClass->IsChildOf(RequiredAncestor)))
			{
				bAllowedInAsset = true;

				break;
			}
		}

		if (!bAllowedInAsset)
		{
			return false;
		}
	}

	return true;
}

bool UFlowAsset::CanFlowAssetReferenceFlowNode(const UClass& FlowNodeClass, FText* OutOptionalFailureReason) const
{
	if (!GEditor || !IsValid(&FlowNodeClass))
	{
		return false;
	}

	// Confirm plugin reference restrictions are being respected
	FAssetReferenceFilterContext AssetReferenceFilterContext;
	AssetReferenceFilterContext.ReferencingAssets.Add(FAssetData(this));
	const TSharedPtr<IAssetReferenceFilter> FlowAssetReferenceFilter = GEditor->MakeAssetReferenceFilter(AssetReferenceFilterContext);
	if (FlowAssetReferenceFilter.IsValid())
	{
		const FAssetData FlowNodeAssetData(&FlowNodeClass);
		if (!FlowAssetReferenceFilter->PassesFilter(FlowNodeAssetData, OutOptionalFailureReason))
		{
			return false;
		}
	}

	return true;
}

UFlowNode* UFlowAsset::CreateNode(const UClass* NodeClass, UEdGraphNode* GraphNode)
{
	UFlowNode* NewNode = NewObject<UFlowNode>(this, NodeClass, NAME_None, RF_Transactional);
	NewNode->SetGraphNode(GraphNode);

	RegisterNode(GraphNode->NodeGuid, NewNode);
	return NewNode;
}

void UFlowAsset::RegisterNode(const FGuid& NewGuid, UFlowNode* NewNode)
{
	NewNode->SetGuid(NewGuid);
	Nodes.Emplace(NewGuid, NewNode);

	HarvestNodeConnections();
}

void UFlowAsset::UnregisterNode(const FGuid& NodeGuid)
{
	Nodes.Remove(NodeGuid);
	Nodes.Compact();

	HarvestNodeConnections();

	MarkPackageDirty();
}

void UFlowAsset::GatherNodeConnections(UEdGraphNode* GraphNode, const UFlowAsset* OwningAsset, TMap<FName, FConnectedPin>& OutInputs, TMap<FName, FPinConnectionList>& OutOutputs)
{
	OutInputs.Empty();
	OutOutputs.Empty();

	for (const UEdGraphPin* Pin : GraphNode->Pins)
	{
		// Process output pins (connections FROM this node)
		if (Pin->Direction == EGPD_Output && Pin->LinkedTo.Num() > 0)
		{
			FName OutputPinName = Pin->PinName;
			auto& [Connections] = OutOutputs.FindOrAdd(OutputPinName);
			for (const UEdGraphPin* LinkedPin : Pin->LinkedTo)
			{
				if (LinkedPin && LinkedPin->GetOwningNode())
				{
					const FGuid TargetNodeGuid = LinkedPin->GetOwningNode()->NodeGuid;
					const FName TargetPinName = LinkedPin->PinName;

					// Ensure the target node exists in the asset
					if (OwningAsset->GetNode(TargetNodeGuid))
					{
						Connections.AddUnique(FConnectedPin(TargetNodeGuid, TargetPinName));
					}
					else
					{
						UE_LOG(LogFlow, Warning, TEXT("Invalid target node '%s' for connection from '%s'"),
						    *TargetNodeGuid.ToString(), *GraphNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
					}
				}
			}
		}
		// Process input pins (connections TO this node)
		else if (Pin->Direction == EGPD_Input && Pin->LinkedTo.Num() > 0)
		{
			const UEdGraphPin* SourcePin = Pin->LinkedTo[0];
			if (SourcePin && SourcePin->GetOwningNode())
			{
				const FGuid SourceNodeGuid = SourcePin->GetOwningNode()->NodeGuid;
				const FName SourcePinName = SourcePin->PinName;

				if (OwningAsset->GetNode(SourceNodeGuid))
				{
					OutInputs.Emplace(Pin->PinName, FConnectedPin(SourceNodeGuid, SourcePinName));
				}
				else
				{
					UE_LOG(LogFlow, Warning, TEXT("Invalid source node '%s' for connection to '%s'"),
					    *SourceNodeGuid.ToString(), *GraphNode->GetNodeTitle(ENodeTitleType::ListView).ToString());
				}
			}
		}
	}
}

bool UFlowAsset::ApplyConnectionChanges(UFlowNode* RuntimeNode, const TMap<FName, FConnectedPin>& NewInputs, const TMap<FName, FPinConnectionList>& NewOutputs)
{
	bool bModified = false;
	bool bOutputsDiffer = false;
	if (RuntimeNode->OutputConnections.Num() != NewOutputs.Num())
	{
		bOutputsDiffer = true;
	}
	else
	{
		for (const auto& Pair : NewOutputs)
		{
			const FPinConnectionList* ExistingList = RuntimeNode->OutputConnections.Find(Pair.Key);
			if (!FlowHarvestHelpers::ArePinConnectionListsEqual(ExistingList, &Pair.Value))
			{
				bOutputsDiffer = true;
				break;
			}
		}
		if (!bOutputsDiffer)
		{
			for (const auto& Pair : RuntimeNode->OutputConnections)
			{
				if (!NewOutputs.Contains(Pair.Key))
				{
					bOutputsDiffer = true;
					break;
				}
			}
		}
	}

	if (bOutputsDiffer)
	{
		RuntimeNode->OutputConnections = NewOutputs;
		bModified = true;
	}

	if (!FlowHarvestHelpers::AreConnectedPinMapsEqual(RuntimeNode->InputConnections, NewInputs))
	{
		RuntimeNode->InputConnections = NewInputs;
		bModified = true;
	}
	
	if (bModified)
	{
		RuntimeNode->SetFlags(RF_Transactional);
		RuntimeNode->Modify();
	}

	return bModified;
}

void UFlowAsset::UpdateTargetNodesInputs(const UFlowAsset* OwningAsset, const FGuid& SourceNodeGuid, const TMap<FName, FPinConnectionList>& Outputs, TSet<UFlowNode*>& OutModifiedNodes)
{
	for (const auto& Pair : Outputs)
	{
		const FName SourcePinName = Pair.Key;
		for (const FConnectedPin& Target : Pair.Value.Connections)
		{
			UFlowNode* TargetNode = OwningAsset->GetNode(Target.NodeGuid);
			if (!TargetNode)
			{
				continue;
			}

			const FName TargetPinName = Target.PinName;
			FConnectedPin SourceInfo(SourceNodeGuid, SourcePinName);
			
			const FConnectedPin* ExistingInput = TargetNode->InputConnections.Find(TargetPinName);
			if (!ExistingInput || *ExistingInput != SourceInfo) // Needs update?
			{
				TargetNode->InputConnections.Emplace(TargetPinName, SourceInfo);
				TargetNode->SetFlags(RF_Transactional);
				TargetNode->Modify();
				OutModifiedNodes.Add(TargetNode);
			}
		}
	}
}

void UFlowAsset::HarvestNodeConnections(UFlowNode* TargetNode)
{
	TArray<UFlowNode*> TargetNodes;
	TSet<UFlowNode*> ModifiedNodes;

	if (IsValid(TargetNode))
	{
		TargetNodes.Reserve(1);
		TargetNodes.Add(TargetNode);
	}
	else
	{
		TargetNodes.Reserve(Nodes.Num());
		for (const TPair<FGuid, UFlowNode*>& Pair : ObjectPtrDecay(Nodes))
		{
			TargetNodes.Add(Pair.Value);
		}
	}

	// Remove any invalid nodes
	for (auto NodeIt = TargetNodes.CreateIterator(); NodeIt; ++NodeIt)
	{
		if (*NodeIt == nullptr)
		{
			NodeIt.RemoveCurrent();
			Modify();
		}
	}

	for (UFlowNode* FlowNode : TargetNodes)
	{
		TMap<FName, FConnectedPin> NewInputs;
		TMap<FName, FPinConnectionList> NewOutputs;
		GatherNodeConnections(FlowNode->GetGraphNode(), this, NewInputs, NewOutputs);
		
		if (ApplyConnectionChanges(FlowNode, NewInputs, NewOutputs))
		{
			ModifiedNodes.Add(FlowNode);
			UpdateTargetNodesInputs(this, FlowNode->GetGraphNode()->NodeGuid, NewOutputs, ModifiedNodes);
		}
	}

	if (ModifiedNodes.Num() > 0)
	{
		SetFlags(RF_Transactional);
		Modify();
		this->Modify();
		this->MarkPackageDirty();
	}
}

#endif

UFlowNode* UFlowAsset::GetDefaultEntryNode() const
{
	UFlowNode* FirstStartNode = nullptr;

	for (const TPair<FGuid, UFlowNode*>& Node : ObjectPtrDecay(Nodes))
	{
		if (UFlowNode_Start* StartNode = Cast<UFlowNode_Start>(Node.Value))
		{
			if (StartNode->GatherConnectedNodes().Num() > 0)
			{
				return StartNode;
			}
			else if (FirstStartNode == nullptr)
			{
				FirstStartNode = StartNode;
			}
		}
	}

	// If none of the found start nodes have connections, fallback to the first start node we found
	return FirstStartNode;
}

#if WITH_EDITOR
void UFlowAsset::AddCustomInput(const FName& EventName)
{
	if (!CustomInputs.Contains(EventName))
	{
		CustomInputs.Add(EventName);
	}
}

void UFlowAsset::RemoveCustomInput(const FName& EventName)
{
	if (CustomInputs.Contains(EventName))
	{
		CustomInputs.Remove(EventName);
	}
}

void UFlowAsset::AddCustomOutput(const FName& EventName)
{
	if (!CustomOutputs.Contains(EventName))
	{
		CustomOutputs.Add(EventName);
	}
}

void UFlowAsset::RemoveCustomOutput(const FName& EventName)
{
	if (CustomOutputs.Contains(EventName))
	{
		CustomOutputs.Remove(EventName);
	}
}
#endif // WITH_EDITOR

UFlowNode_CustomInput* UFlowAsset::TryFindCustomInputNodeByEventName(const FName& EventName) const
{
	for (const TPair<FGuid, UFlowNode*>& Node : ObjectPtrDecay(Nodes))
	{
		if (UFlowNode_CustomInput* CustomInput = Cast<UFlowNode_CustomInput>(Node.Value))
		{
			if (CustomInput->GetEventName() == EventName)
			{
				return CustomInput;
			}
		}
	}

	return nullptr;
}

UFlowNode_CustomOutput* UFlowAsset::TryFindCustomOutputNodeByEventName(const FName& EventName) const
{
	for (const TPair<FGuid, UFlowNode*>& Node : ObjectPtrDecay(Nodes))
	{
		if (UFlowNode_CustomOutput* CustomOutput = Cast<UFlowNode_CustomOutput>(Node.Value))
		{
			if (CustomOutput->GetEventName() == EventName)
			{
				return CustomOutput;
			}
		}
	}

	return nullptr;
}

TArray<FName> UFlowAsset::GatherCustomInputNodeEventNames() const
{
	// Runtime-safe gathering of the CustomInputs (which is editor-only data)
	//  from the actual flow nodes
	TArray<FName> Results;

	for (const TPair<FGuid, UFlowNode*>& Node : ObjectPtrDecay(Nodes))
	{
		if (UFlowNode_CustomInput* CustomInput = Cast<UFlowNode_CustomInput>(Node.Value))
		{
			Results.Add(CustomInput->GetEventName());
		}
	}

	return Results;
}

TArray<FName> UFlowAsset::GatherCustomOutputNodeEventNames() const
{
	// Runtime-safe gathering of the CustomOutputs (which is editor-only data)
	//  from the actual flow nodes
	TArray<FName> Results;

	for (const TPair<FGuid, UFlowNode*>& Node : ObjectPtrDecay(Nodes))
	{
		if (UFlowNode_CustomOutput* CustomOutput = Cast<UFlowNode_CustomOutput>(Node.Value))
		{
			Results.Add(CustomOutput->GetEventName());
		}
	}

	return Results;
}

TArray<UFlowNode*> UFlowAsset::GetNodesInExecutionOrder(UFlowNode* FirstIteratedNode, const TSubclassOf<UFlowNode> FlowNodeClass)
{
	TArray<UFlowNode*> FoundNodes;
	GetNodesInExecutionOrder<UFlowNode>(FirstIteratedNode, FoundNodes);

	// filter out nodes by class
	for (int32 i = FoundNodes.Num() - 1; i >= 0; i--)
	{
		if (!FoundNodes[i]->GetClass()->IsChildOf(FlowNodeClass))
		{
			FoundNodes.RemoveAt(i);
		}
	}
	FoundNodes.Shrink();

	return FoundNodes;
}

TArray<UFlowNode*> UFlowAsset::GatherNodesConnectedToAllInputs() const
{
	TSet<TObjectKey<UFlowNode>> IteratedNodes;
	TArray<UFlowNode*> ConnectedNodes;

	// Nodes connected to the Start node
	UFlowNode* DefaultEntryNode = GetDefaultEntryNode();
	GetNodesInExecutionOrder_Recursive(DefaultEntryNode, IteratedNodes, ConnectedNodes);

	// Nodes connected to Custom Input node(s)
	for (const TPair<FGuid, UFlowNode*>& Node : ObjectPtrDecay(Nodes))
	{
		if (UFlowNode_CustomInput* CustomInput = Cast<UFlowNode_CustomInput>(Node.Value))
		{
			GetNodesInExecutionOrder_Recursive(CustomInput, IteratedNodes, ConnectedNodes);
		}
	}

	return ConnectedNodes;
}

void UFlowAsset::AddInstance(UFlowAsset* Instance)
{
	ActiveInstances.Add(Instance);
}

int32 UFlowAsset::RemoveInstance(UFlowAsset* Instance)
{
#if WITH_EDITOR
	if (InspectedInstance.IsValid() && InspectedInstance.Get() == Instance)
	{
		SetInspectedInstance(NAME_None);
	}
#endif

	ActiveInstances.Remove(Instance);
	return ActiveInstances.Num();
}

void UFlowAsset::ClearInstances()
{
#if WITH_EDITOR
	if (InspectedInstance.IsValid())
	{
		SetInspectedInstance(NAME_None);
	}
#endif

	for (int32 i = ActiveInstances.Num() - 1; i >= 0; i--)
	{
		if (ActiveInstances.IsValidIndex(i) && ActiveInstances[i])
		{
			ActiveInstances[i]->FinishFlow(EFlowFinishPolicy::Keep);
		}
	}

	ActiveInstances.Empty();
}

#if WITH_EDITOR
void UFlowAsset::GetInstanceDisplayNames(TArray<TSharedPtr<FName>>& OutDisplayNames) const
{
	for (const UFlowAsset* Instance : ActiveInstances)
	{
		OutDisplayNames.Emplace(MakeShareable(new FName(Instance->GetDisplayName())));
	}
}

void UFlowAsset::SetInspectedInstance(const FName& NewInspectedInstanceName)
{
	if (NewInspectedInstanceName.IsNone())
	{
		InspectedInstance = nullptr;
	}
	else
	{
		for (UFlowAsset* ActiveInstance : ActiveInstances)
		{
			if (ActiveInstance && ActiveInstance->GetDisplayName() == NewInspectedInstanceName)
			{
				if (!InspectedInstance.IsValid() || InspectedInstance != ActiveInstance)
				{
					InspectedInstance = ActiveInstance;
				}
				break;
			}
		}
	}

	BroadcastDebuggerRefresh();
}

void UFlowAsset::BroadcastDebuggerRefresh() const
{
	RefreshDebuggerEvent.Broadcast();
}

void UFlowAsset::BroadcastRuntimeMessageAdded(const TSharedRef<FTokenizedMessage>& Message) const
{
	RuntimeMessageEvent.Broadcast(this, Message);
}
#endif // WITH_EDITOR

void UFlowAsset::InitializeInstance(const TWeakObjectPtr<UObject> InOwner, UFlowAsset& InTemplateAsset)
{
	check(!IsInstanceInitialized());

	Owner = InOwner;
	TemplateAsset = &InTemplateAsset;

	for (TPair<FGuid, TObjectPtr<UFlowNode>>& Node : Nodes)
	{
		UFlowNode* NewNodeInstance = NewObject<UFlowNode>(this, Node.Value->GetClass(), NAME_None, RF_Transient, Node.Value, false, nullptr);
		Node.Value = NewNodeInstance;

		if (UFlowNode_CustomInput* CustomInput = Cast<UFlowNode_CustomInput>(NewNodeInstance))
		{
			if (!CustomInput->EventName.IsNone())
			{
				CustomInputNodes.Emplace(CustomInput);
			}
		}

		NewNodeInstance->InitializeInstance();
	}
}

void UFlowAsset::DeinitializeInstance()
{
	if (IsInstanceInitialized())
	{
		for (const TPair<FGuid, UFlowNode*>& Node : ObjectPtrDecay(Nodes))
		{
			if (IsValid(Node.Value))
			{
				Node.Value->DeinitializeInstance();
			}
		}

		const int32 ActiveInstancesLeft = TemplateAsset->RemoveInstance(this);
		if (ActiveInstancesLeft == 0 && GetFlowSubsystem())
		{
			GetFlowSubsystem()->RemoveInstancedTemplate(TemplateAsset);
		}

		TemplateAsset = nullptr;
	}
}

void UFlowAsset::PreStartFlow()
{
	ResetNodes();

#if WITH_EDITOR
	check(IsInstanceInitialized());

	if (TemplateAsset->ActiveInstances.Num() == 1)
	{
		// this instance is the only active one, set it directly as Inspected Instance
		TemplateAsset->SetInspectedInstance(GetDisplayName());
	}
	else
	{
		// request to refresh list to show newly created instance
		TemplateAsset->BroadcastDebuggerRefresh();
	}
#endif
}

void UFlowAsset::StartFlow()
{
	PreStartFlow();

	if (UFlowNode* ConnectedEntryNode = GetDefaultEntryNode())
	{
		RecordedNodes.Add(ConnectedEntryNode);
		ConnectedEntryNode->TriggerFirstOutput(true);
	}
}

void UFlowAsset::FinishFlow(const EFlowFinishPolicy InFinishPolicy, const bool bRemoveInstance /*= true*/)
{
	FinishPolicy = InFinishPolicy;

	// end execution of this asset and all of its nodes
	for (UFlowNode* Node : ActiveNodes)
	{
		Node->Deactivate();
	}
	ActiveNodes.Empty();

	// flush preloaded content
	for (UFlowNode* PreloadedNode : PreloadedNodes)
	{
		PreloadedNode->TriggerFlush();
	}
	PreloadedNodes.Empty();

	// provides option to finish game-specific logic prior to removing asset instance
	if (bRemoveInstance)
	{
		DeinitializeInstance();
	}
}

bool UFlowAsset::HasStartedFlow() const
{
	return RecordedNodes.Num() > 0;
}

AActor* UFlowAsset::TryFindActorOwner() const
{
	const UActorComponent* OwnerAsComponent = Cast<UActorComponent>(GetOwner());
	if (IsValid(OwnerAsComponent))
	{
		return Cast<AActor>(OwnerAsComponent->GetOwner());
	}

	return nullptr;
}

TWeakObjectPtr<UFlowAsset> UFlowAsset::GetFlowInstance(UFlowNode_SubGraph* SubGraphNode) const
{
	return ActiveSubGraphs.FindRef(SubGraphNode);
}

void UFlowAsset::TriggerCustomInput_FromSubGraph(UFlowNode_SubGraph* SubGraphNode, const FName& EventName) const
{
	const TWeakObjectPtr<UFlowAsset> FlowInstance = ActiveSubGraphs.FindRef(SubGraphNode);
	if (FlowInstance.IsValid())
	{
		FlowInstance->TriggerCustomInput(EventName);
	}
}

void UFlowAsset::TriggerCustomInput(const FName& EventName)
{
	for (UFlowNode_CustomInput* CustomInputNode : CustomInputNodes)
	{
		if (CustomInputNode->EventName == EventName)
		{
			RecordedNodes.Add(CustomInputNode);
			CustomInputNode->ExecuteInput(EventName);
		}
	}
}

void UFlowAsset::TriggerCustomOutput(const FName& EventName)
{
	if (NodeOwningThisAssetInstance.IsValid())
	{
		// it's a SubGraph
		NodeOwningThisAssetInstance->TriggerOutput(EventName);
	}
	else
	{
		// it's a Root Flow, so the intention here might be to call event on the Flow Component
		if (UFlowComponent* FlowComponent = Cast<UFlowComponent>(GetOwner()))
		{
			FlowComponent->DispatchRootFlowCustomEvent(this, EventName);
		}
	}
}

void UFlowAsset::TriggerInput(const FGuid& NodeGuid, const FName& PinName)
{
	if (UFlowNode* Node = Nodes.FindRef(NodeGuid))
	{
		if (!ActiveNodes.Contains(Node))
		{
			ActiveNodes.Add(Node);
			RecordedNodes.Add(Node);
		}

		Node->TriggerInput(PinName);
	}
}

void UFlowAsset::FinishNode(UFlowNode* Node)
{
	if (ActiveNodes.Contains(Node))
	{
		ActiveNodes.Remove(Node);

		// if graph reached Finish and this asset instance was created by SubGraph node
		if (Node->CanFinishGraph())
		{
			if (NodeOwningThisAssetInstance.IsValid())
			{
				NodeOwningThisAssetInstance.Get()->TriggerFirstOutput(true);

				return;
			}

			// if this instance is a Root Flow, we need to deregister it from the subsystem first
			if (Owner.IsValid())
			{
				const TSet<UFlowAsset*>& RootFlowInstances = GetFlowSubsystem()->GetRootInstancesByOwner(Owner.Get());
				if (RootFlowInstances.Contains(this))
				{
					GetFlowSubsystem()->FinishRootFlow(Owner.Get(), TemplateAsset, EFlowFinishPolicy::Keep);

					return;
				}
			}

			FinishFlow(EFlowFinishPolicy::Keep);
		}
	}
}

void UFlowAsset::ResetNodes()
{
	for (UFlowNode* Node : RecordedNodes)
	{
		Node->ResetRecords();
	}

	RecordedNodes.Empty();
}

UFlowSubsystem* UFlowAsset::GetFlowSubsystem() const
{
	return Cast<UFlowSubsystem>(GetOuter());
}

FName UFlowAsset::GetDisplayName() const
{
	return GetFName();
}

UFlowNode_SubGraph* UFlowAsset::GetNodeOwningThisAssetInstance() const
{
	return NodeOwningThisAssetInstance.Get();
}

UFlowAsset* UFlowAsset::GetParentInstance() const
{
	return NodeOwningThisAssetInstance.IsValid() ? NodeOwningThisAssetInstance.Get()->GetFlowAsset() : nullptr;
}

FFlowAssetSaveData UFlowAsset::SaveInstance(TArray<FFlowAssetSaveData>& SavedFlowInstances)
{
	FFlowAssetSaveData AssetRecord;
	AssetRecord.WorldName = IsBoundToWorld() ? GetWorld()->GetName() : FString();
	AssetRecord.InstanceName = GetName();

	// opportunity to collect data before serializing asset
	OnSave();

	// iterate nodes
	TArray<UFlowNode*> NodesInExecutionOrder;
	GetNodesInExecutionOrder<UFlowNode>(GetDefaultEntryNode(), NodesInExecutionOrder);
	for (UFlowNode* Node : NodesInExecutionOrder)
	{
		if (Node && Node->ActivationState == EFlowNodeState::Active)
		{
			// iterate SubGraphs
			if (UFlowNode_SubGraph* SubGraphNode = Cast<UFlowNode_SubGraph>(Node))
			{
				const TWeakObjectPtr<UFlowAsset> SubFlowInstance = GetFlowInstance(SubGraphNode);
				if (SubFlowInstance.IsValid())
				{
					const FFlowAssetSaveData SubAssetRecord = SubFlowInstance->SaveInstance(SavedFlowInstances);
					SubGraphNode->SavedAssetInstanceName = SubAssetRecord.InstanceName;
				}
			}

			FFlowNodeSaveData NodeRecord;
			Node->SaveInstance(NodeRecord);

			AssetRecord.NodeRecords.Emplace(NodeRecord);
		}
	}

	// serialize asset
	FMemoryWriter MemoryWriter(AssetRecord.AssetData, true);
	FFlowArchive Ar(MemoryWriter);
	Serialize(Ar);

	// write archive to SaveGame
	SavedFlowInstances.Emplace(AssetRecord);

	return AssetRecord;
}

void UFlowAsset::LoadInstance(const FFlowAssetSaveData& AssetRecord)
{
	FMemoryReader MemoryReader(AssetRecord.AssetData, true);
	FFlowArchive Ar(MemoryReader);
	Serialize(Ar);

	PreStartFlow();

	// iterate graph "from the end", backward to execution order
	// prevents issue when the preceding node would instantly fire output to a not-yet-loaded node
	for (int32 i = AssetRecord.NodeRecords.Num() - 1; i >= 0; i--)
	{
		if (UFlowNode* Node = Nodes.FindRef(AssetRecord.NodeRecords[i].NodeGuid))
		{
			Node->LoadInstance(AssetRecord.NodeRecords[i]);
		}
	}

	OnLoad();
}

void UFlowAsset::OnActivationStateLoaded(UFlowNode* Node)
{
	if (Node->ActivationState != EFlowNodeState::NeverActivated)
	{
		RecordedNodes.Emplace(Node);
	}

	if (Node->ActivationState == EFlowNodeState::Active)
	{
		ActiveNodes.Emplace(Node);
	}
}

void UFlowAsset::OnSave_Implementation()
{
}

void UFlowAsset::OnLoad_Implementation()
{
}

bool UFlowAsset::IsBoundToWorld_Implementation()
{
	return bWorldBound;
}

#if WITH_EDITOR
void UFlowAsset::LogError(const FString& MessageToLog, const UFlowNodeBase* Node) const
{
	// this is runtime log which is should be only called on runtime instances of asset
	if (TemplateAsset)
	{
		UE_LOG(LogFlow, Log, TEXT("Attempted to use Runtime Log on asset instance %s"), *MessageToLog);
	}

	if (RuntimeLog.Get())
	{
		const TSharedRef<FTokenizedMessage> TokenizedMessage = RuntimeLog.Get()->Error(*MessageToLog, Node);
		BroadcastRuntimeMessageAdded(TokenizedMessage);
	}
}

void UFlowAsset::LogWarning(const FString& MessageToLog, const UFlowNodeBase* Node) const
{
	// this is runtime log which is should be only called on runtime instances of asset
	if (TemplateAsset)
	{
		UE_LOG(LogFlow, Log, TEXT("Attempted to use Runtime Log on asset instance %s"), *MessageToLog);
	}

	if (RuntimeLog.Get())
	{
		const TSharedRef<FTokenizedMessage> TokenizedMessage = RuntimeLog.Get()->Warning(*MessageToLog, Node);
		BroadcastRuntimeMessageAdded(TokenizedMessage);
	}
}

void UFlowAsset::LogNote(const FString& MessageToLog, const UFlowNodeBase* Node) const
{
	// this is runtime log which is should be only called on runtime instances of asset
	if (TemplateAsset)
	{
		UE_LOG(LogFlow, Log, TEXT("Attempted to use Runtime Log on asset instance %s"), *MessageToLog);
	}

	if (RuntimeLog.Get())
	{
		const TSharedRef<FTokenizedMessage> TokenizedMessage = RuntimeLog.Get()->Note(*MessageToLog, Node);
		BroadcastRuntimeMessageAdded(TokenizedMessage);
	}
}
#endif

TSharedPtr<FJsonObject> UFlowAsset::SerializeAddOn(const UFlowNodeAddOn* AddOn, EFlowAssetJSONSerializationMode Mode)
{
	TSharedPtr<FJsonObject> AddOnJson = MakeShared<FJsonObject>();

	// serialize type
	AddOnJson->SetStringField(TEXT("Type"), AddOn->GetClass()->GetName());

	if (Mode == EFlowAssetJSONSerializationMode::Verbose)
	{
		// Input pins
		TArray<TSharedPtr<FJsonValue>> InputNames;
		for (const FFlowPin& Pin : AddOn->GetInputPins())
		{
			InputNames.Add(MakeShared<FJsonValueString>(Pin.PinName.ToString()));
		}
		AddOnJson->SetArrayField(TEXT("InputPins"), InputNames);

		// Output pins
		//TArray<TSharedPtr<FJsonValue>> OutputNames;
		//for (const FFlowPin& Pin : AddOn->GetOutputPins())
		//{
		//	OutputNames.Add(MakeShared<FJsonValueString>(Pin.PinName.ToString()));
		//}
		//AddOnJson->SetArrayField(TEXT("OutputPins"), OutputNames);
	}
	else if (Mode == EFlowAssetJSONSerializationMode::Minimal)
	{
		// In Minimal mode, NumPins field as "NumInputs.NumOutputs"
		const int32 NumInputs = AddOn->GetInputPins().Num();
		const int32 NumOutputs = 0;//AddOn->GetOutputPins().Num();
		AddOnJson->SetStringField(TEXT("NumPins"), FString::Printf(TEXT("%d.%d"), NumInputs, NumOutputs));
	}

	// Serialize editable, non-editor-only properties (excluding UFlowNodeBase and its parents)
	TArray<TSharedPtr<FJsonValue>> PropertiesArray;
	for (TFieldIterator<FProperty> PropIt(AddOn->GetClass()); PropIt; ++PropIt)
	{
		FProperty* Property = *PropIt;
		if (Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible) &&
			!Property->HasAnyPropertyFlags(CPF_Transient | CPF_EditorOnly) &&
			Property->GetOwnerClass() != UFlowNodeBase::StaticClass() &&
			Property->GetOwnerClass()->IsChildOf(UFlowNodeBase::StaticClass()))
		{
			TSharedPtr<FJsonValue> JsonValue = FJsonObjectConverter::UPropertyToJsonValue(
				Property,
				Property->ContainerPtrToValuePtr<void>(AddOn),
				CPF_Edit | CPF_BlueprintVisible,
				CPF_Transient | CPF_EditorOnly
			);

			TSharedPtr<FJsonObject> PropJson = MakeShared<FJsonObject>();
			PropJson->SetField(Property->GetName(), JsonValue);
			PropertiesArray.Add(MakeShared<FJsonValueObject>(PropJson));
		}
	}
	AddOnJson->SetArrayField(TEXT("Properties"), PropertiesArray);

	// Serialize AddOns
	TArray<TSharedPtr<FJsonValue>> AddOnsArray;
	AddOn->ForEachAddOnConst([&AddOnsArray, Mode](const UFlowNodeAddOn& ChildAddOn)
	{
		AddOnsArray.Add(MakeShared<FJsonValueObject>(SerializeAddOn(&ChildAddOn, Mode)));
		return EFlowForEachAddOnFunctionReturnValue::Continue;
	}, EFlowForEachAddOnChildRule::ImmediateChildrenOnly);
	if (AddOnsArray.Num())
	{
		AddOnJson->SetArrayField(TEXT("AddOns"), AddOnsArray);
	}

	return AddOnJson;
}

TSharedPtr<FJsonObject> UFlowAsset::SerializeNode(const UFlowNode* Node, EFlowAssetJSONSerializationMode Mode)
{
	TSharedPtr<FJsonObject> NodeJson = MakeShared<FJsonObject>();

	// Always serialize type
	NodeJson->SetStringField(TEXT("Type"), Node->GetClass()->GetName());

	// Serialize name only in Verbose mode
	if (Mode == EFlowAssetJSONSerializationMode::Verbose)
	{
		NodeJson->SetStringField(TEXT("Name"), Node->GetName());

		// Input pins
		TArray<TSharedPtr<FJsonValue>> InputNames;
		for (const FFlowPin& Pin : Node->GetInputPins())
		{
			InputNames.Add(MakeShared<FJsonValueString>(Pin.PinName.ToString()));
		}
		NodeJson->SetArrayField(TEXT("InputPins"), InputNames);

		// Output pins
		TArray<TSharedPtr<FJsonValue>> OutputNames;
		for (const FFlowPin& Pin : Node->GetOutputPins())
		{
			OutputNames.Add(MakeShared<FJsonValueString>(Pin.PinName.ToString()));
		}
		NodeJson->SetArrayField(TEXT("OutputPins"), OutputNames);
	}
	else if (Mode == EFlowAssetJSONSerializationMode::Minimal)
	{
		// In Minimal mode, NumPins field as "NumInputs.NumOutputs"
		const int32 NumInputs = Node->GetInputPins().Num();
		const int32 NumOutputs = Node->GetOutputPins().Num();
		NodeJson->SetStringField(TEXT("NumPins"), FString::Printf(TEXT("%d.%d"), NumInputs, NumOutputs));
	}

	// Editable properties (excluding UFlowNode and its parents)
	TArray<TSharedPtr<FJsonValue>> PropertiesArray;
	for (TFieldIterator<FProperty> PropIt(Node->GetClass()); PropIt; ++PropIt)
	{
		FProperty* Property = *PropIt;
		if (Property->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible) &&
			!Property->HasAnyPropertyFlags(CPF_Transient | CPF_EditorOnly) &&
			Property->GetOwnerClass() != UFlowNode::StaticClass() &&
			Property->GetOwnerClass()->IsChildOf(UFlowNode::StaticClass()))
		{
			TSharedPtr<FJsonValue> JsonValue = FJsonObjectConverter::UPropertyToJsonValue(
				Property,
				Property->ContainerPtrToValuePtr<void>(Node),
				CPF_Edit | CPF_BlueprintVisible,
				CPF_Transient | CPF_EditorOnly
			);

			TSharedPtr<FJsonObject> PropJson = MakeShared<FJsonObject>();
			PropJson->SetField(Property->GetName(), JsonValue);
			PropertiesArray.Add(MakeShared<FJsonValueObject>(PropJson));
		}
	}
	NodeJson->SetArrayField(TEXT("Properties"), PropertiesArray);

	// Serialize AddOns
	TArray<TSharedPtr<FJsonValue>> AddOnsArray;
	Node->ForEachAddOnConst([&AddOnsArray, Mode](const UFlowNodeAddOn& AddOn)
	{
		AddOnsArray.Add(MakeShared<FJsonValueObject>(SerializeAddOn(&AddOn, Mode)));
		return EFlowForEachAddOnFunctionReturnValue::Continue;
	}, EFlowForEachAddOnChildRule::ImmediateChildrenOnly);
	if (AddOnsArray.Num())
	{
		NodeJson->SetArrayField(TEXT("AddOns"), AddOnsArray);
	}

	return NodeJson;
}

FString UFlowAsset::FlowAssetToJSON(EFlowAssetJSONSerializationMode Mode) const
{
	TSharedPtr<FJsonObject> RootJson = MakeShared<FJsonObject>();

	auto GraphVariablesProp = GetClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UFlowAsset, GraphVariables));
	RootJson->SetField(TEXT("GraphVariables"), FJsonObjectConverter::UPropertyToJsonValue(GraphVariablesProp, GraphVariablesProp->ContainerPtrToValuePtr<void>(this)));

	// Serialize Nodes
	TArray<TSharedPtr<FJsonValue>> NodesArray;
	TArray<const UFlowNode*> NodeList;
	for (const auto& NodePair : GetNodes())
	{
		if (NodePair.Value)
		{
			NodesArray.Add(MakeShared<FJsonValueObject>(SerializeNode(NodePair.Value, Mode)));
			NodeList.Add(NodePair.Value);
		}
	}
	RootJson->SetArrayField(TEXT("Nodes"), NodesArray);

	// Serialize Edges
	TArray<TSharedPtr<FJsonValue>> EdgesArray;
	TMap<const UFlowNode*, int32> NodeToIndex;
	for (int32 i = 0; i < NodeList.Num(); ++i)
	{
		NodeToIndex.Add(NodeList[i], i);
	}

	if (Mode == EFlowAssetJSONSerializationMode::Minimal)
	{
		// Minimal: [ "0.0", "1.0" ]
		for (int32 SourceIdx = 0; SourceIdx < NodeList.Num(); ++SourceIdx)
		{
			const UFlowNode* SourceNode = NodeList[SourceIdx];
			for (int32 OutputPinIdx = 0; OutputPinIdx < SourceNode->GetOutputPins().Num(); ++OutputPinIdx)
			{
				const FFlowPin& OutputPin = SourceNode->GetOutputPins()[OutputPinIdx];
				const TArray<FConnectedPin> Connections = SourceNode->GetOutputConnections(OutputPin.PinName);

				for (const FConnectedPin& Conn : Connections)
				{
					const UFlowNode* TargetNode = GetNode(Conn.NodeGuid);
					int32 TargetIdx = NodeList.IndexOfByKey(TargetNode);
					if (TargetIdx != INDEX_NONE)
					{
						// Find input pin index
						int32 InputPinIdx = -1;
						const TArray<FFlowPin>& TargetInputPins = TargetNode->GetInputPins();
						for (int32 i = 0; i < TargetInputPins.Num(); ++i)
						{
							if (TargetInputPins[i].PinName == Conn.PinName)
							{
								InputPinIdx = i;
								break;
							}
						}
						if (InputPinIdx != -1)
						{
							TArray<TSharedPtr<FJsonValue>> EdgeArray;
							EdgeArray.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%d.%d"), SourceIdx, OutputPinIdx)));
							EdgeArray.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%d.%d"), TargetIdx, InputPinIdx)));
							EdgesArray.Add(MakeShared<FJsonValueArray>(EdgeArray));
						}
					}
				}
			}
		}
	}
	else // Verbose
	{
		for (int32 SourceIdx = 0; SourceIdx < NodeList.Num(); ++SourceIdx)
		{
			const UFlowNode* SourceNode = NodeList[SourceIdx];
			for (int32 OutputPinIdx = 0; OutputPinIdx < SourceNode->GetOutputPins().Num(); ++OutputPinIdx)
			{
				const FFlowPin& OutputPin = SourceNode->GetOutputPins()[OutputPinIdx];
				const TArray<FConnectedPin> Connections = SourceNode->GetOutputConnections(OutputPin.PinName);

				for (const FConnectedPin& Conn : Connections)
				{
					const UFlowNode* TargetNode = GetNode(Conn.NodeGuid);
					if (TargetNode)
					{
						TSharedPtr<FJsonObject> EdgeObj = MakeShared<FJsonObject>();
						EdgeObj->SetStringField(TEXT("From"), SourceNode->GetName());
						EdgeObj->SetStringField(TEXT("FromPin"), OutputPin.PinName.ToString());
						EdgeObj->SetStringField(TEXT("To"), TargetNode->GetName());
						EdgeObj->SetStringField(TEXT("ToPin"), Conn.PinName.ToString());

						EdgesArray.Add(MakeShared<FJsonValueObject>(EdgeObj));
					}
				}
			}
		}
	}
	RootJson->SetArrayField(TEXT("Edges"), EdgesArray);

	FString OutputString;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputString);
	FJsonSerializer::Serialize(RootJson.ToSharedRef(), Writer);
	return OutputString;
}

UFlowNodeAddOn* UFlowAsset::DeserializeAddOn(const TSharedPtr<FJsonObject>& AddOnJson, UObject* Outer, EFlowAssetJSONSerializationMode Mode)
{
	FString AddOnType;
	if (!AddOnJson->TryGetStringField(TEXT("Type"), AddOnType))
	{
		return nullptr;
	}

	UClass* AddOnClass = FindFirstObject<UClass>(*AddOnType);
	if (!AddOnClass || !AddOnClass->IsChildOf(UFlowNodeAddOn::StaticClass()))
	{
		UE_LOG(LogTemp, Warning, TEXT("Failed to find UClass for AddOn type %s"), *AddOnType);
		return nullptr;
	}

	FName AddOnName = NAME_None;
	if (Mode == EFlowAssetJSONSerializationMode::Verbose)
	{
		FString NameStr;
		if (AddOnJson->TryGetStringField(TEXT("Name"), NameStr))
		{
			AddOnName = FName(*NameStr);
		}
	}

	UFlowNodeAddOn* AddOn = NewObject<UFlowNodeAddOn>(Outer, AddOnClass, AddOnName, RF_Transactional);

	// Deserialize editable properties
	const TArray<TSharedPtr<FJsonValue>>* PropertiesArrayPtr;
	if (AddOnJson->TryGetArrayField(TEXT("Properties"), PropertiesArrayPtr))
	{
		for (const TSharedPtr<FJsonValue>& PropValue : *PropertiesArrayPtr)
		{
			const TSharedPtr<FJsonObject>* PropJsonPtr;
			if (PropValue->TryGetObject(PropJsonPtr))
			{
				for (const auto& PropPair : (*PropJsonPtr)->Values)
				{
					FProperty* Property = AddOnClass->FindPropertyByName(FName(*PropPair.Key));
					if (Property)
					{
						FJsonObjectConverter::JsonValueToUProperty(
							PropPair.Value,
							Property,
							Property->ContainerPtrToValuePtr<void>(AddOn),
							0, 0
						);
					}
				}
			}
		}
	}

	// Recursively deserialize child AddOns
	const TArray<TSharedPtr<FJsonValue>>* AddOnsArrayPtr;
	if (AddOnJson->TryGetArrayField(TEXT("AddOns"), AddOnsArrayPtr))
	{
		for (const TSharedPtr<FJsonValue>& ChildAddOnValue : *AddOnsArrayPtr)
		{
			const TSharedPtr<FJsonObject>* ChildAddOnJsonPtr;
			if (ChildAddOnValue->TryGetObject(ChildAddOnJsonPtr))
			{
				UFlowNodeAddOn* ChildAddOn = DeserializeAddOn(*ChildAddOnJsonPtr, AddOn, Mode);
				if (ChildAddOn)
				{
					AddOn->AddOns.Add(ChildAddOn);
				}
			}
		}
	}

	return AddOn;
}

UFlowNode* UFlowAsset::DeserializeNode(const TSharedPtr<FJsonObject>& NodeJson, UFlowAsset* AssetOuter, EFlowAssetJSONSerializationMode Mode)
{
	FString NodeType;
	NodeJson->TryGetStringField(TEXT("Type"), NodeType);

	// Find UClass for NodeType
	UClass* NodeClass = FindFirstObject<UClass>(*NodeType);
	if (!NodeClass)
	{
		UE_LOG(LogTemp, Warning, TEXT("Failed to find UClass for %s"), *NodeType);
		return nullptr;
	}

	// Node name only present in Verbose mode
	FName NodeName;
	if (Mode == EFlowAssetJSONSerializationMode::Verbose)
	{
		FString NodeNameStr;
		NodeJson->TryGetStringField(TEXT("Name"), NodeNameStr);
		NodeName = *NodeNameStr;
	}
	else
	{
		NodeName = NAME_None;
	}

	// Create node instance
	UFlowNode* Node = NewObject<UFlowNode>(AssetOuter, NodeClass, NodeName, RF_Transient);

	// Deserialize editable properties
	const TArray<TSharedPtr<FJsonValue>>* PropertiesArrayPtr;
	if (NodeJson->TryGetArrayField(TEXT("Properties"), PropertiesArrayPtr))
	{
		for (const TSharedPtr<FJsonValue>& PropValue : *PropertiesArrayPtr)
		{
			const TSharedPtr<FJsonObject>* PropJsonPtr;
			if (PropValue->TryGetObject(PropJsonPtr))
			{
				for (const auto& PropPair : (*PropJsonPtr)->Values)
				{
					FProperty* Property = NodeClass->FindPropertyByName(FName(*PropPair.Key));
					if (Property)
					{
						FJsonObjectConverter::JsonValueToUProperty(
							PropPair.Value,
							Property,
							Property->ContainerPtrToValuePtr<void>(Node),
							0, 0
						);
					}
				}
			}
		}
	}

	// Deserialize AddOns
	const TArray<TSharedPtr<FJsonValue>>* AddOnsArrayPtr;
	if (NodeJson->TryGetArrayField(TEXT("AddOns"), AddOnsArrayPtr))
	{
		for (const TSharedPtr<FJsonValue>& AddOnValue : *AddOnsArrayPtr)
		{
			const TSharedPtr<FJsonObject>* AddOnJsonPtr;
			if (AddOnValue->TryGetObject(AddOnJsonPtr))
			{
				UFlowNodeAddOn* AddOn = DeserializeAddOn(*AddOnJsonPtr, Node, Mode);
				if (AddOn)
				{
					Node->AddOns.Add(AddOn);
				}
			}
		}
	}

	return Node;
}

UFlowAsset* UFlowAsset::FlowAssetFromJSON(const FString& JsonString, EFlowAssetJSONSerializationMode Mode)
{
	TSharedPtr<FJsonObject> RootJson;
	TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonString);
	if (!FJsonSerializer::Deserialize(Reader, RootJson) || !RootJson.IsValid())
	{
		return nullptr;
	}

	// Create new FlowAsset
	UFlowAsset* FlowAsset = NewObject<UFlowAsset>(GetTransientPackage(), NAME_None, RF_Transient);

	// --- Deserialize GraphVariables ---
	if (TSharedPtr<FJsonValue> GraphVarsJson = RootJson->TryGetField(TEXT("GraphVariables")))
	{
		auto GraphVariablesProp = FlowAsset->GetClass()->FindPropertyByName(GET_MEMBER_NAME_CHECKED(UFlowAsset, GraphVariables));
		bool bSuccess = FJsonObjectConverter::JsonValueToUProperty(GraphVarsJson, GraphVariablesProp, GraphVariablesProp->ContainerPtrToValuePtr<void>(FlowAsset));
		if (!bSuccess)
		{
			UE_LOG(LogTemp, Warning, TEXT("Failed to deserialize UFlowAsset::GraphVariables"));
		}
	}

	// --- Deserialize Nodes ---
	const TArray<TSharedPtr<FJsonValue>>* NodesArrayPtr;
	TArray<UFlowNode*> NodeList;
	TMap<FString, UFlowNode*> NameToNode;
	if (RootJson->TryGetArrayField(TEXT("Nodes"), NodesArrayPtr))
	{
		for (const TSharedPtr<FJsonValue>& NodeValue : *NodesArrayPtr)
		{
			const TSharedPtr<FJsonObject>* NodeJsonPtr;
			if (NodeValue->TryGetObject(NodeJsonPtr))
			{
				UFlowNode* Node = DeserializeNode(*NodeJsonPtr, FlowAsset, Mode);
				if (!Node) continue;

				FGuid NodeGuid = FGuid::NewGuid();
				Node->SetGuid(NodeGuid);
				FlowAsset->Nodes.Add(NodeGuid, Node);

				NodeList.Add(Node);
				if (Mode == EFlowAssetJSONSerializationMode::Verbose)
				{
					NameToNode.Add(Node->GetName(), Node);
				}
			}
		}
	}

	// --- Deserialize Edges ---
	const TArray<TSharedPtr<FJsonValue>>* EdgesArrayPtr;
	if (RootJson->TryGetArrayField(TEXT("Edges"), EdgesArrayPtr))
	{
		if (Mode == EFlowAssetJSONSerializationMode::Minimal)
		{
			// Build pin name maps for each node
			TArray<TArray<FName>> NodeInputPinNames;
			TArray<TArray<FName>> NodeOutputPinNames;
			for (UFlowNode* Node : NodeList)
			{
				TArray<FName> InputPins, OutputPins;
				for (const FFlowPin& Pin : Node->GetInputPins())
				{
					InputPins.Add(Pin.PinName);
				}
				for (const FFlowPin& Pin : Node->GetOutputPins())
				{
					OutputPins.Add(Pin.PinName);
				}
				NodeInputPinNames.Add(InputPins);
				NodeOutputPinNames.Add(OutputPins);
			}

			for (const TSharedPtr<FJsonValue>& EdgeValue : *EdgesArrayPtr)
			{
				const TArray<TSharedPtr<FJsonValue>>* EdgeArrayPtr;
				if (EdgeValue->TryGetArray(EdgeArrayPtr) && EdgeArrayPtr->Num() == 2)
				{
					FString SourceStr, TargetStr;
					(*EdgeArrayPtr)[0]->TryGetString(SourceStr);
					(*EdgeArrayPtr)[1]->TryGetString(TargetStr);

					int32 SourceNodeIdx = 0, SourcePinIdx = 0, TargetNodeIdx = 0, TargetPinIdx = 0;
					{
						TArray<FString> Parts;
						SourceStr.ParseIntoArray(Parts, TEXT("."));
						if (Parts.Num() == 2)
						{
							SourceNodeIdx = FCString::Atoi(*Parts[0]);
							SourcePinIdx = FCString::Atoi(*Parts[1]);
						}
					}
					{
						TArray<FString> Parts;
						TargetStr.ParseIntoArray(Parts, TEXT("."));
						if (Parts.Num() == 2)
						{
							TargetNodeIdx = FCString::Atoi(*Parts[0]);
							TargetPinIdx = FCString::Atoi(*Parts[1]);
						}
					}

					if (NodeList.IsValidIndex(SourceNodeIdx) && NodeList.IsValidIndex(TargetNodeIdx) &&
						NodeOutputPinNames[SourceNodeIdx].IsValidIndex(SourcePinIdx) &&
						NodeInputPinNames[TargetNodeIdx].IsValidIndex(TargetPinIdx))
					{
						UFlowNode* SourceNode = NodeList[SourceNodeIdx];
						UFlowNode* TargetNode = NodeList[TargetNodeIdx];
						FName FromPin = NodeOutputPinNames[SourceNodeIdx][SourcePinIdx];
						FName ToPin = NodeInputPinNames[TargetNodeIdx][TargetPinIdx];

						// Connect SourceNode's OutputPin to TargetNode's InputPin
						FConnectedPin Conn(TargetNode->GetGuid(), ToPin);
						FPinConnectionList& OutList = SourceNode->OutputConnections.FindOrAdd(FromPin);
						OutList.Connections.Add(Conn);

						SourceNode->Modify();

						// Set input connection on target node
						TargetNode->InputConnections.Add(ToPin, FConnectedPin(SourceNode->GetGuid(), FromPin));
						TargetNode->Modify();
					}
				}
			}
		}
		else // Verbose
		{
			for (const TSharedPtr<FJsonValue>& EdgeValue : *EdgesArrayPtr)
			{
				const TSharedPtr<FJsonObject>* EdgeObjPtr;
				if (EdgeValue->TryGetObject(EdgeObjPtr))
				{
					FString FromName, FromPin, ToName, ToPin;
					(*EdgeObjPtr)->TryGetStringField(TEXT("From"), FromName);
					(*EdgeObjPtr)->TryGetStringField(TEXT("FromPin"), FromPin);
					(*EdgeObjPtr)->TryGetStringField(TEXT("To"), ToName);
					(*EdgeObjPtr)->TryGetStringField(TEXT("ToPin"), ToPin);

					UFlowNode* SourceNode = NameToNode.FindRef(FromName);
					UFlowNode* TargetNode = NameToNode.FindRef(ToName);

					if (SourceNode && TargetNode)
					{
						// Connect SourceNode's OutputPin to TargetNode's InputPin
						FConnectedPin Conn(TargetNode->GetGuid(), FName(*ToPin));
						FPinConnectionList& OutList = SourceNode->OutputConnections.FindOrAdd(FName(*FromPin));
						OutList.Connections.Add(Conn);

						SourceNode->Modify();

						// Set input connection on target node
						TargetNode->InputConnections.Add(FName(*ToPin), FConnectedPin(SourceNode->GetGuid(), FName(*FromPin)));
						TargetNode->Modify();
					}
				}
			}
		}
	}

#if WITH_EDITOR
	if (IFlowEditorModuleInterface* Extension = FModuleManager::Get().LoadModulePtr<IFlowEditorModuleInterface>(TEXT("FlowEditor")))
	{
		Extension->DeserializeEdGraphFromJSON(FlowAsset, RootJson, Mode);
	}
#endif

	FFlowMessageLog LogResults;
	FlowAsset->ValidateAsset(LogResults);
	for (const auto& Message : LogResults.Messages)
	{
		UE_LOG(LogTemp, Warning, TEXT("%s"), *Message->ToText().ToString());
	}
	return FlowAsset;
}
