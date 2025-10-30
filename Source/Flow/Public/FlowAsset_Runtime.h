// Copyright https://github.com/MothCocoon/FlowGraph/graphs/contributors

#pragma once

#include "FlowAsset.h"
#include "FlowAsset_Runtime.generated.h"

class UFlowNode;

/**
 * Builder class for creating FlowAssets at runtime
 * Provides a fluent API for constructing flow graphs programmatically
 */
class FLOW_API FFlowAssetRuntimeBuilder
{
public:
	FFlowAssetRuntimeBuilder(UFlowAsset* InAsset);
	
	/**
	 * Add a node to the flow graph
	 * @param NodeClass The class of the node to create
	 * @param OutNodeGuid The GUID of the created node (for connecting)
	 * @return Reference to this builder for chaining
	 */
	FFlowAssetRuntimeBuilder& AddNode(TSubclassOf<UFlowNode> NodeClass, FGuid& OutNodeGuid);
	
	/**
	 * Add a node to the flow graph with custom GUID
	 * @param NodeClass The class of the node to create
	 * @param NodeGuid The GUID to assign to the node
	 * @return Reference to this builder for chaining
	 */
	FFlowAssetRuntimeBuilder& AddNodeWithGuid(TSubclassOf<UFlowNode> NodeClass, const FGuid& NodeGuid);
	
	/**
	 * Connect two nodes via their output/input pins
	 * @param FromNode GUID of the source node
	 * @param FromPin Name of the output pin
	 * @param ToNode GUID of the target node
	 * @param ToPin Name of the input pin
	 * @return Reference to this builder for chaining
	 */
	FFlowAssetRuntimeBuilder& Connect(const FGuid& FromNode, FName FromPin, const FGuid& ToNode, FName ToPin);
	
	/**
	 * Connect two nodes via their default pins (Out -> In)
	 * @param FromNode GUID of the source node
	 * @param ToNode GUID of the target node
	 * @return Reference to this builder for chaining
	 */
	FFlowAssetRuntimeBuilder& ConnectDefault(const FGuid& FromNode, const FGuid& ToNode);
	
	/**
	 * Get the created node by GUID (useful for configuring node properties)
	 * @param NodeGuid GUID of the node to retrieve
	 * @return The node, or nullptr if not found
	 */
	UFlowNode* GetNode(const FGuid& NodeGuid) const;
	
	/**
	 * Get the created node by GUID with type casting
	 * @param NodeGuid GUID of the node to retrieve
	 * @return The node cast to the specified type, or nullptr if not found or wrong type
	 */
	template<typename T>
	T* GetNode(const FGuid& NodeGuid) const
	{
		static_assert(TPointerIsConvertibleFromTo<T, const UFlowNode>::Value, "'T' must be derived from UFlowNode");
		return Cast<T>(GetNode(NodeGuid));
	}
	
	/**
	 * Complete the build and return the constructed FlowAsset
	 * @return The flow asset that was built
	 */
	UFlowAsset* Build();
	
private:
	UFlowAsset* Asset;
};

/**
 * Specialized FlowAsset designed for runtime generation
 * This class is optimized for programmatic creation without requiring editor graphs
 */
UCLASS(BlueprintType)
class FLOW_API UFlowAsset_Runtime : public UFlowAsset
{
	GENERATED_UCLASS_BODY()
	
public:
	/**
	 * Create a new runtime builder for this asset
	 * @return A builder instance for constructing the graph
	 */
	FFlowAssetRuntimeBuilder CreateBuilder();
	
	/**
	 * Static factory method to create a runtime FlowAsset with builder
	 * @param WorldContextObject The outer object (usually a UWorld or game instance)
	 * @param InOwner The owner object for this Flow instance (e.g., World Settings or Player Controller)
	 * @param AssetName Optional name for the asset
	 * @return A new runtime FlowAsset instance
	 */
	UFUNCTION(BlueprintCallable, Category = "FlowAsset|Runtime", meta = (WorldContext = "WorldContextObject", DefaultToSelf = "InOwner"))
	static UFlowAsset_Runtime* CreateRuntimeFlowAsset(UObject* WorldContextObject, FName AssetName, UObject* InOwner);
};
