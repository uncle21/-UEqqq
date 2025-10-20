// Copyright Epic Games, Inc. All Rights Reserved.

#include "MovieGraphImageSequenceOutputNode.h"

#include "Graph/Nodes/MovieGraphGlobalOutputSettingNode.h"
#include "Graph/Nodes/MovieGraphRenderLayerNode.h"
#include "Graph/MovieGraphDataTypes.h"
#include "Graph/MovieGraphPipeline.h"
#include "Graph/MovieGraphConfig.h"
#include "Graph/MovieGraphFilenameResolveParams.h"
#include "Graph/MovieGraphBlueprintLibrary.h"
#include "MoviePipelineUtils.h"
#include "MoviePipelineImageSequenceOutput.h" // for FAsyncImageQuantization
#include "MovieRenderPipelineCoreModule.h"

#include "Modules/ModuleManager.h"
#include "ImageWriteQueue.h"
#include "Misc/Paths.h"
#include "Async/TaskGraphInterfaces.h"
#include "Graph/MovieGraphOCIOHelper.h"

UMovieGraphImageSequenceOutputNode::UMovieGraphImageSequenceOutputNode()
{
	ImageWriteQueue = &FModuleManager::Get().LoadModuleChecked<IImageWriteQueueModule>("ImageWriteQueue").GetWriteQueue();
}

void UMovieGraphImageSequenceOutputNode::OnAllFramesSubmittedImpl(UMovieGraphPipeline* InPipeline, TObjectPtr<UMovieGraphEvaluatedConfig>& InPrimaryJobEvaluatedGraph)
{
	FinalizeFence = ImageWriteQueue->CreateFence();
}

bool UMovieGraphImageSequenceOutputNode::IsFinishedWritingToDiskImpl() const
{
	// Wait until the finalization fence is reached meaning we've written everything to disk.
	return Super::IsFinishedWritingToDiskImpl() && (!FinalizeFence.IsValid() || FinalizeFence.WaitFor(0));
}

FString UMovieGraphImageSequenceOutputNode::CreateFileName(
	UE::MovieGraph::FMovieGraphOutputMergerFrame* InRawFrameData,
	const UMovieGraphImageSequenceOutputNode* InParentNode,
	const UMovieGraphPipeline* InPipeline,
	const TPair<FMovieGraphRenderDataIdentifier, TUniquePtr<FImagePixelData>>& InRenderData,
	const EImageFormat InImageFormat,
	FMovieGraphResolveArgs& OutMergedFormatArgs) const
{
	const TCHAR* Extension = TEXT("");
	switch (InImageFormat)
	{
	case EImageFormat::PNG: Extension = TEXT("png"); break;
	case EImageFormat::JPEG: Extension = TEXT("jpeg"); break;
	case EImageFormat::BMP: Extension = TEXT("bmp"); break;
	case EImageFormat::EXR: Extension = TEXT("exr"); break;
	}

	UMovieGraphGlobalOutputSettingNode* OutputSettingNode = InRawFrameData->EvaluatedConfig->GetSettingForBranch<UMovieGraphGlobalOutputSettingNode>(GlobalsPinName);
	if (!OutputSettingNode)
	{
		return FString();
	}

	// Generate one string that puts the directory combined with the filename format.
	FString FileNameFormatString = OutputSettingNode->OutputDirectory.Path / InParentNode->FileNameFormat;

	// Insert tokens like {layer_name} as appropriate to make sure outputs don't clash with each other.
	DisambiguateFilename(FileNameFormatString, InRawFrameData, InParentNode->GetFName(), InRenderData);

	// Previous method is preserved for output frame number validation.
	constexpr bool bIncludeRenderPass = false;
	constexpr bool bTestFrameNumber = true;
	constexpr bool bIncludeCameraName = false;
	UE::MoviePipeline::ValidateOutputFormatString(FileNameFormatString, bIncludeRenderPass, bTestFrameNumber, bIncludeCameraName);
	
	// Map the .ext to be specific to our output data.
	TMap<FString, FString> AdditionalFormatArgs;
	AdditionalFormatArgs.Add(TEXT("ext"), Extension);

	UE::MovieGraph::FMovieGraphSampleState* Payload = InRenderData.Value->GetPayload<UE::MovieGraph::FMovieGraphSampleState>();

	FMovieGraphFilenameResolveParams Params = FMovieGraphFilenameResolveParams::MakeResolveParams(
		InRenderData.Key,
		InPipeline,
		InRawFrameData->EvaluatedConfig.Get(),
		Payload->TraversalContext,
		AdditionalFormatArgs);

	// Take our string path from the Output Setting and resolve it.
	return UMovieGraphBlueprintLibrary::ResolveFilenameFormatArguments(FileNameFormatString, Params, OutMergedFormatArgs);
}

void UMovieGraphImageSequenceOutputNode::OnReceiveImageDataImpl(UMovieGraphPipeline* InPipeline, UE::MovieGraph::FMovieGraphOutputMergerFrame* InRawFrameData, const TSet<FMovieGraphRenderDataIdentifier>& InMask)
{
	check(InRawFrameData);

	TArray<TPair<FMovieGraphRenderDataIdentifier, TUniquePtr<FImagePixelData>>> CompositedPasses = GetCompositedPasses(InRawFrameData);

	// ToDo:
	// The ImageWriteQueue is set up in a fire-and-forget manner. This means that the data needs to be placed in the WriteQueue
	// as a TUniquePtr (so it can free the data when its done). Unfortunately if we have multiple output formats at once,
	// we can't MoveTemp the data so we need to make a copy.
	// 
	// Copying can be expensive (3ms @ 1080p, 12ms at 4k for a single layer image) so ideally we'd like to do it on the task graph
	// but this isn't really compatible with the ImageWriteQueue API as we need the future returned by the ImageWriteQueue to happen
	// in order, so that we push our futures to the main Movie Pipeline in order, otherwise when we encode files to videos they'll
	// end up with frames out of order. A workaround for this would be to chain all of the send-to-imagewritequeue tasks to each
	// other with dependencies, but I'm not sure that's going to scale to the potentialy high data volume going wide MRQ will eventually
	// need.

	// The base ImageSequenceOutputNode doesn't support any multilayer formats, so we write out each render pass separately.
	for (TPair<FMovieGraphRenderDataIdentifier, TUniquePtr<FImagePixelData>>& RenderData : InRawFrameData->ImageOutputData)
	{
		// If this pass is composited, skip it for now
		if (CompositedPasses.ContainsByPredicate([&RenderData](const TPair<FMovieGraphRenderDataIdentifier, TUniquePtr<FImagePixelData>>& CompositedPass)
			{
				return CompositedPass.Key == RenderData.Key;
			}))
		{
			continue;
		}
		
		// A layer within this output data may have chosen to not be written to disk by this CDO node
		if (!InMask.Contains(RenderData.Key))
		{
			continue;
		}

		checkf(RenderData.Value.IsValid(), TEXT("Unexpected empty image data: incorrectly moved or its production failed?"));

		// ToDo: Certain images may require transparency, at which point
		// we write out a .png instead of a .jpeg.
		EImageFormat PreferredOutputFormat = OutputFormat;

		constexpr bool bIncludeCDOs = false;
		constexpr bool bExactMatch = true;
		const UMovieGraphImageSequenceOutputNode* ParentNode = Cast<UMovieGraphImageSequenceOutputNode>(
			InRawFrameData->EvaluatedConfig->GetSettingForBranch(GetClass(), RenderData.Key.RootBranchName, bIncludeCDOs, bExactMatch));
		checkf(ParentNode, TEXT("Image sequence output should not exist without a parent node in the graph."));
		
		FMovieGraphResolveArgs FinalResolvedKVPs;
		FString FileName = CreateFileName(InRawFrameData, ParentNode, InPipeline, RenderData, PreferredOutputFormat, FinalResolvedKVPs);
		if (!ensureMsgf(!FileName.IsEmpty(), TEXT("Unexpected empty file name, skipping frame.")))
		{
			continue;
		}

		TUniquePtr<FImageWriteTask> TileImageTask = MakeUnique<FImageWriteTask>();
		TileImageTask->Format = PreferredOutputFormat;
		TileImageTask->CompressionQuality = 100;
		TileImageTask->Filename = FileName;

		// Pixel data can only be moved if there are no other active output image sequence nodes on the branch
		if (GetNumFileOutputNodes(*InRawFrameData->EvaluatedConfig, RenderData.Key.RootBranchName) > 1)
		{
			TileImageTask->PixelData = RenderData.Value->CopyImageData();
		}
		else
		{
			TileImageTask->PixelData = RenderData.Value->MoveImageDataToNew();
		}

		UE::MovieGraph::FMovieGraphSampleState* Payload = RenderData.Value->GetPayload<UE::MovieGraph::FMovieGraphSampleState>();
		
		bool bQuantizationEncodeSRGB = true;
#if WITH_OCIO
		if (FMovieGraphOCIOHelper::GenerateOcioPixelPreProcessor(Payload, InPipeline, InRawFrameData->EvaluatedConfig.Get(), ParentNode->OCIOConfiguration, ParentNode->OCIOContext, TileImageTask->PixelPreProcessors))
		{
			// We assume that any encoding on the output transform should be done by OCIO
			bQuantizationEncodeSRGB = false;
		}
#endif // WITH_OCIO

		EImagePixelType PixelType = TileImageTask->PixelData->GetType();

		if (bQuantizeTo8Bit && TileImageTask->PixelData->GetBitDepth() > 8u)
		{
			TileImageTask->PixelPreProcessors.Emplace(UE::MoviePipeline::FAsyncImageQuantization(TileImageTask.Get(), bQuantizationEncodeSRGB));
			PixelType = EImagePixelType::Color;
		}

		// Perform compositing if any composited passes were found earlier
		for (TPair<FMovieGraphRenderDataIdentifier, TUniquePtr<FImagePixelData>>& CompositedPass : CompositedPasses)
		{
			// This pass may not allow other passes to be composited on it
			if (!Payload->bAllowsCompositing)
			{
				continue;
			}
			
			// This composited pass will only composite on top of renders w/ the same branch and camera
			if (!CompositedPass.Key.IsBranchAndCameraEqual(RenderData.Key))
			{
				continue;
			}

			// There could be multiple renders within this branch using the composited pass, so we have to copy the image data
			switch (PixelType)
			{
			case EImagePixelType::Color:
				TileImageTask->PixelPreProcessors.Add(TAsyncCompositeImage<FColor>(CompositedPass.Value->CopyImageData()));
				break;
			case EImagePixelType::Float16:
				TileImageTask->PixelPreProcessors.Add(TAsyncCompositeImage<FFloat16Color>(CompositedPass.Value->CopyImageData()));
				break;
			case EImagePixelType::Float32:
				TileImageTask->PixelPreProcessors.Add(TAsyncCompositeImage<FLinearColor>(CompositedPass.Value->CopyImageData()));
				break;
			}
		}

		UE::MovieGraph::FMovieGraphOutputFutureData OutputData;
		OutputData.Shot = InPipeline->GetActiveShotList()[Payload->TraversalContext.ShotIndex];
		OutputData.FilePath = FileName;
		OutputData.DataIdentifier = RenderData.Key;

		TFuture<bool> Future = ImageWriteQueue->Enqueue(MoveTemp(TileImageTask));

		InPipeline->AddOutputFuture(MoveTemp(Future), OutputData);
	}
}


TUniquePtr<FEXRImageWriteTask> UMovieGraphImageSequenceOutputNode_EXR::CreateImageWriteTask(FString InFileName, EEXRCompressionFormat InCompression) const
{
	// Ensure our OpenExrRTTI module gets loaded.
	UE_CALL_ONCE([]
		{
			check(IsInGameThread());
			FModuleManager::Get().LoadModule(TEXT("UEOpenExrRTTI"));
		});

	TUniquePtr<FEXRImageWriteTask> ImageWriteTask = MakeUnique<FEXRImageWriteTask>();
	ImageWriteTask->Filename = MoveTemp(InFileName);
	ImageWriteTask->Compression = InCompression;
	// ImageWriteTask->CompressionLevel is intentionally skipped and not exposed ("dwaCompressionLevel" is deprecated)

	return MoveTemp(ImageWriteTask);
}

void UMovieGraphImageSequenceOutputNode_EXR::PrepareTaskGlobalMetadata(FEXRImageWriteTask& InOutImageTask, UE::MovieGraph::FMovieGraphOutputMergerFrame* InRawFrameData, TMap<FString, FString>& InMetadata) const
{
	// Add in hardware usage metadata
	UE::MoviePipeline::GetHardwareUsageMetadata(InMetadata, FPaths::GetPath(InOutImageTask.Filename));

	// Add passed in resolved metadata
	for (const TPair<FString, FString>& Metadata : InMetadata)
	{
		InOutImageTask.FileMetadata.Emplace(Metadata.Key, Metadata.Value);
	}

	// Add in any metadata from the output merger frame
	for (const TPair<FString, FString>& Metadata : InRawFrameData->FileMetadata)
	{
		InOutImageTask.FileMetadata.Add(Metadata.Key, Metadata.Value);
	}
}

void UMovieGraphImageSequenceOutputNode_EXR::UpdateTaskPerLayer(
	FEXRImageWriteTask& InOutImageTask,
	const UMovieGraphImageSequenceOutputNode* InParentNode,
	TUniquePtr<FImagePixelData> InImageData,
	int32 InLayerIndex,
	const FString& InLayerName,
	const TMap<FString, FString>& InResolvedOCIOContext) const
{
	const UE::MovieGraph::FMovieGraphSampleState* Payload = InImageData->GetPayload<UE::MovieGraph::FMovieGraphSampleState>();

	bool bEnabledOCIO = false;
#if WITH_OCIO
	if (FMovieGraphOCIOHelper::GenerateOcioPixelPreProcessorWithContext(Payload, InParentNode->OCIOConfiguration, InResolvedOCIOContext, InOutImageTask.PixelPreprocessors.FindOrAdd(InLayerIndex)))
	{
		bEnabledOCIO = true;
	}
#endif // WITH_OCIO

	if (InLayerIndex == 0)
	{
		// Add task information that is common to all layers. This metadata may be redundant with unreal/* metadata,
		// but these are "standard" fields in EXR metadata.
		InOutImageTask.FileMetadata.Add("owner", UE::MoviePipeline::GetJobAuthor(Payload->TraversalContext.Job));
		InOutImageTask.FileMetadata.Add("comments", Payload->TraversalContext.Job->Comment);

		const FIntPoint& Resolution = InImageData->GetSize();
		InOutImageTask.Width = Resolution.X;
		InOutImageTask.Height = Resolution.Y;

		InOutImageTask.OverscanPercentage = Payload->OverscanFraction;
#if WITH_OCIO
		if (bEnabledOCIO)
		{
			UE::MoviePipeline::UpdateColorSpaceMetadata(InParentNode->OCIOConfiguration.ColorConfiguration, InOutImageTask);
		}
		else
#endif // WITH_OCIO
		{
			UE::MoviePipeline::UpdateColorSpaceMetadata(Payload->SceneCaptureSource, InOutImageTask);
		}
	}

	if (!InLayerName.IsEmpty())
	{
		InOutImageTask.LayerNames.FindOrAdd(InImageData.Get(), InLayerName);
	}

	InOutImageTask.Layers.Add(MoveTemp(InImageData));
}

void UMovieGraphImageSequenceOutputNode_EXR::OnReceiveImageDataImpl(UMovieGraphPipeline* InPipeline, UE::MovieGraph::FMovieGraphOutputMergerFrame* InRawFrameData, const TSet<FMovieGraphRenderDataIdentifier>& InMask)
{
	check(InRawFrameData);

	TArray<TPair<FMovieGraphRenderDataIdentifier, TUniquePtr<FImagePixelData>>> CompositedPasses = GetCompositedPasses(InRawFrameData);

	for (TPair<FMovieGraphRenderDataIdentifier, TUniquePtr<FImagePixelData>>& RenderData : InRawFrameData->ImageOutputData)
	{
		// If this pass is composited, skip it for now
		if (CompositedPasses.ContainsByPredicate([&RenderData](const TPair<FMovieGraphRenderDataIdentifier, TUniquePtr<FImagePixelData>>& CompositedPass)
			{
				return RenderData.Key == CompositedPass.Key;
			}))
		{
			continue;
		}

		// A layer within this output data may have chosen to not be written to disk by this CDO node
		if (!InMask.Contains(RenderData.Key))
		{
			continue;
		}

		checkf(RenderData.Value.IsValid(), TEXT("Unexpected empty image data: incorrectly moved or its production failed?"));

		constexpr bool bIncludeCDOs = false;
		constexpr bool bExactMatch = true;
		const UMovieGraphImageSequenceOutputNode_EXR* ParentNode = InRawFrameData->EvaluatedConfig->GetSettingForBranch<UMovieGraphImageSequenceOutputNode_EXR>(
			RenderData.Key.RootBranchName, bIncludeCDOs, bExactMatch);
		checkf(ParentNode, TEXT("Single-layer EXR should not exist without a parent node in the graph."));

		FMovieGraphResolveArgs ResolvedFormatArgs;
		FString FileName = CreateFileName(InRawFrameData, ParentNode, InPipeline, RenderData, OutputFormat, ResolvedFormatArgs);
		if (!ensureMsgf(!FileName.IsEmpty(), TEXT("Unexpected empty file name, skipping frame.")))
		{
			continue;
		}

		UE::MovieGraph::FMovieGraphSampleState* Payload = RenderData.Value->GetPayload<UE::MovieGraph::FMovieGraphSampleState>();
		
		TUniquePtr<FEXRImageWriteTask> ImageWriteTask = CreateImageWriteTask(FileName, ParentNode->Compression);
		
		PrepareTaskGlobalMetadata(*ImageWriteTask, InRawFrameData, ResolvedFormatArgs.FileMetadata);

		// No layer is equivalent to a zero-index layer
		constexpr int32 LayerIndex = 0;
		TUniquePtr<FImagePixelData> PixelData;
		if (GetNumFileOutputNodes(*InRawFrameData->EvaluatedConfig, RenderData.Key.RootBranchName) > 1)
		{
			PixelData = RenderData.Value->CopyImageData();
		}
		else
		{
			PixelData = RenderData.Value->MoveImageDataToNew();
		}

		TMap<FString, FString> ResolvedOCIOContext = {};
#if WITH_OCIO
		ResolvedOCIOContext = FMovieGraphOCIOHelper::ResolveOpenColorIOContext(
			ParentNode->OCIOContext,
			RenderData.Key,
			InPipeline,
			InRawFrameData->EvaluatedConfig.Get(),
			Payload->TraversalContext
		);
#endif // WITH_OCIO

		UpdateTaskPerLayer(*ImageWriteTask, ParentNode, MoveTemp(PixelData), LayerIndex, FString(), ResolvedOCIOContext);

		// Perform compositing if any composited passes were found earlier
		for (TPair<FMovieGraphRenderDataIdentifier, TUniquePtr<FImagePixelData>>& CompositedPass : CompositedPasses)
		{
			// This pass may not allow other passes to be composited on it
			if (!Payload->bAllowsCompositing)
			{
				continue;
			}
			
			// This composited pass will only composite on top of renders w/ the same branch and camera
			if (CompositedPass.Key.IsBranchAndCameraEqual(RenderData.Key))
			{
				EImagePixelType PixelType = RenderData.Value->GetType();

				// There could be multiple renders within this branch using the composited pass, so we have to copy the image data
				switch (PixelType)
				{
				case EImagePixelType::Color:
					ImageWriteTask->PixelPreprocessors.FindOrAdd(LayerIndex).Add(TAsyncCompositeImage<FColor>(CompositedPass.Value->CopyImageData()));
					break;
				case EImagePixelType::Float16:
					ImageWriteTask->PixelPreprocessors.FindOrAdd(LayerIndex).Add(TAsyncCompositeImage<FFloat16Color>(CompositedPass.Value->CopyImageData()));
					break;
				case EImagePixelType::Float32:
					ImageWriteTask->PixelPreprocessors.FindOrAdd(LayerIndex).Add(TAsyncCompositeImage<FLinearColor>(CompositedPass.Value->CopyImageData()));
					break;
				}
			}
		}

		UE::MovieGraph::FMovieGraphOutputFutureData OutputFutureData;
		OutputFutureData.Shot = InPipeline->GetActiveShotList()[Payload->TraversalContext.ShotIndex];
		OutputFutureData.FilePath = FileName;
		OutputFutureData.DataIdentifier = RenderData.Key;

		TFuture<bool> Future = ImageWriteQueue->Enqueue(MoveTemp(ImageWriteTask));

		InPipeline->AddOutputFuture(MoveTemp(Future), OutputFutureData);
	}
}


void UMovieGraphImageSequenceOutputNode_MultiLayerEXR::OnReceiveImageDataImpl(UMovieGraphPipeline* InPipeline, UE::MovieGraph::FMovieGraphOutputMergerFrame* InRawFrameData, const TSet<FMovieGraphRenderDataIdentifier>& InMask)
{
	check(InRawFrameData);

	constexpr bool bIncludeCDOs = false;
	constexpr bool bExactMatch = true;
	const UMovieGraphImageSequenceOutputNode_MultiLayerEXR* ParentNode = InRawFrameData->EvaluatedConfig->GetSettingForBranch<UMovieGraphImageSequenceOutputNode_MultiLayerEXR>(
		UMovieGraphNode::GlobalsPinName, bIncludeCDOs, bExactMatch);
	checkf(ParentNode, TEXT("Multi-Layer EXR should not exist without a parent node in the graph."));

	// Generate a mapping of resolved filename -> RenderIDs, and filename -> resolve args. The generated EXRs can only
	// store layers with a common resolution, and this takes care of ensuring that only renderIDs with a common resolution
	// map to the same filename.
	TMap<FString, TArray<FMovieGraphRenderDataIdentifier>> FilenameToRenderIDs;
	TMap<FString, FMovieGraphResolveArgs> FilenameToResolveArgs;
	GetFilenameToRenderIDMappings(ParentNode, InPipeline, InRawFrameData, FilenameToRenderIDs, FilenameToResolveArgs);

	// Write an EXR for each filename, which potentially contains multiple passes (render IDs).
	for (const TPair<FString, TArray<FMovieGraphRenderDataIdentifier>>& RenderIDsForFilename : FilenameToRenderIDs)
	{
		const FString& Filename = RenderIDsForFilename.Key;
		const TArray<FMovieGraphRenderDataIdentifier>& RenderIDs = RenderIDsForFilename.Value;
		
		TUniquePtr<FEXRImageWriteTask> MultiLayerImageTask = CreateImageWriteTask(Filename, ParentNode->Compression);
		PrepareTaskGlobalMetadata(*MultiLayerImageTask, InRawFrameData, FilenameToResolveArgs[Filename].FileMetadata);

		// Add each render pass as a layer to the EXR
		int32 LayerIndex = 0;
		int32 ShotIndex = 0;
		for (const FMovieGraphRenderDataIdentifier& RenderID : RenderIDs)
		{
			const TUniquePtr<FImagePixelData>& ImageData = InRawFrameData->ImageOutputData[RenderID];
			checkf(ImageData.IsValid(), TEXT("Unexpected empty image data: incorrectly moved or its production failed?"));

			const UE::MovieGraph::FMovieGraphSampleState* Payload = ImageData->GetPayload<UE::MovieGraph::FMovieGraphSampleState>();
			ShotIndex = Payload->TraversalContext.ShotIndex;

			// The first layer doesn't get an explicit name. However, subsequent layers may have an explicit name specified for them. Normally layer
			// names are procedurally generated though (below).
			FString LayerName = (LayerIndex != 0) ? Payload->LayerNameOverride : FString();

			// Generate a procedural layer name if an explicit name wasn't specified.
			if ((LayerIndex != 0) && LayerName.IsEmpty())
			{
				// If there is more than one layer, then we will prefix the layer. The first layer is not prefixed (and gets inserted as RGBA)
				// as most programs that handle EXRs expect the main image data to be in an unnamed layer. We only postfix with cameraname
				// if there's multiple cameras, as pipelines may be already be built around the generic "one camera" support.
				// TODO: The number of cameras may be inaccurate -- no camera setting in the graph yet
				UMoviePipelineExecutorShot* CurrentShot = InPipeline->GetActiveShotList()[ShotIndex];
				int32 NumCameras = CurrentShot->SidecarCameras.Num();

				UE::MovieGraph::FMovieGraphRenderDataValidationInfo ValidationInfo = InRawFrameData->GetValidationInfo(RenderID, /*bInDiscardCompositedRenders*/ false);
				TArray<FString> Tokens;

				if (ValidationInfo.BranchCount > 1)
				{
					if (ValidationInfo.LayerCount < ValidationInfo.BranchCount)
					{
						Tokens.Add(RenderID.RootBranchName.ToString());
					}
					else
					{
						Tokens.Add(RenderID.LayerName);
					}
				}

				if (ValidationInfo.ActiveBranchRendererCount > 1)
				{
					Tokens.Add(RenderID.RendererName);
				}

				if (ValidationInfo.ActiveRendererSubresourceCount > 1)
				{
					Tokens.Add(RenderID.SubResourceName);
				}

				if (NumCameras > 1)
				{
					Tokens.Add(RenderID.CameraName);
				}

				if (ensureMsgf(!Tokens.IsEmpty(), TEXT("Missing expected EXR layer token.")))
				{
					LayerName = Tokens[0];
					
					for (int32 Index = 1; Index < Tokens.Num(); ++Index)
					{
						LayerName = FString::Printf(TEXT("%s_%s"), *LayerName, *Tokens[Index]);
					}
				}
			}

			TUniquePtr<FImagePixelData> PixelData;
			if (GetNumFileOutputNodes(*InRawFrameData->EvaluatedConfig, RenderID.RootBranchName) > 1)
			{
				PixelData = ImageData->CopyImageData();
			}
			else
			{
				PixelData = ImageData->MoveImageDataToNew();
			}

			TMap<FString, FString> ResolvedOCIOContext = {};
#if WITH_OCIO
			ResolvedOCIOContext = FMovieGraphOCIOHelper::ResolveOpenColorIOContext(
				ParentNode->OCIOContext,
				RenderID,
				InPipeline,
				InRawFrameData->EvaluatedConfig.Get(),
				Payload->TraversalContext
			);
#endif // WITH_OCIO
			UpdateTaskPerLayer(*MultiLayerImageTask, ParentNode, MoveTemp(PixelData), LayerIndex, LayerName, ResolvedOCIOContext);

			LayerIndex++;
		}
		
		UE::MovieGraph::FMovieGraphOutputFutureData OutputFutureData;
		OutputFutureData.Shot = InPipeline->GetActiveShotList()[ShotIndex];
		OutputFutureData.FilePath = Filename;
		OutputFutureData.DataIdentifier = FMovieGraphRenderDataIdentifier(); // EXRs put all the render passes internally so this resolves to a ""

		InPipeline->AddOutputFuture(ImageWriteQueue->Enqueue(MoveTemp(MultiLayerImageTask)), OutputFutureData);
	}
}

void UMovieGraphImageSequenceOutputNode_MultiLayerEXR::GetFilenameToRenderIDMappings(
	const UMovieGraphImageSequenceOutputNode_MultiLayerEXR* InParentNode,
	UMovieGraphPipeline* InPipeline, UE::MovieGraph::FMovieGraphOutputMergerFrame* InRawFrameData,
	TMap<FString, TArray<FMovieGraphRenderDataIdentifier>>& OutFilenameToRenderIDs,
	TMap<FString, FMovieGraphResolveArgs>& OutFilenameToResolveArgs) const
{
	// Merge one layer's resolve args (InNewResolveArgs) into an existing set of resolve args (InExistingResolveArgs).
	auto MergeResolveArgs = [](FMovieGraphResolveArgs& InNewResolveArgs, FMovieGraphResolveArgs& InExistingResolveArgs)
	{
		// Covert the filename arguments to FormatNamedArguments once; this is needed by FString::Format() in the loop
		FStringFormatNamedArguments NamedArguments;
		for (const TPair<FString, FString>& FilenameArgument : InNewResolveArgs.FilenameArguments)
		{
			NamedArguments.Add(FilenameArgument.Key, FilenameArgument.Value);
		}

		for (TPair<FString, FString>& MetadataPair : InNewResolveArgs.FileMetadata)
		{
			// The metadata key and/or value may contain filename format {tokens}; resolve any of them BEFORE merging in with existing metadata. This
			// is important because the metadata may contain a {token} that, once resolved, prevents a collision with an existing key.
			MetadataPair.Key = FString::Format(*MetadataPair.Key, NamedArguments);
			MetadataPair.Value = FString::Format(*MetadataPair.Value, NamedArguments);

			// Merge in the resolved metadata into the existing metadata
			InExistingResolveArgs.FileMetadata.Add(MetadataPair.Key, MetadataPair.Value);
		}

		// The filename arguments are not needed after merging + resolving; however, the last set of arguments is passed along anyway if they are needed.
		// They aren't merged though, because they differ too much between layers to make merging of any practical usefulness (eg, {layer_name}).
		InExistingResolveArgs.FilenameArguments = InNewResolveArgs.FilenameArguments;
	};
	
	TMap<FString, TArray<FIntPoint>> FilenameToResolutions;

	// First, generate filename -> renderID mapping, and filename -> resolution mapping.
	// This assumes that all render passes will have the same resolution, so we use 0 as the resolution index.
	// Once we know the resolutions of all the render passes, they can be binned together into groups with the same
	// resolution, and the filenames can be regenerated to ensure that passes of differing resolutions go to
	// different files.
	//
	// This two-step process is necessary due to the flexibility in file naming, and the multi-layer nature of EXRs.
	// For example, if the file name format is "{sequence_name}.{frame_number}", and the second of two branches in the
	// graph has a differing resolution, only after resolving the output filenames for all outputs is a problem found;
	// layers of differing resolutions will be written to the same file. Using "{layer_name}.{sequence_name}.{frame_number}"
	// as the file name format would prevent the issue, but the two-step process is a generic way of approaching the
	// problem.
	for (const TPair<FMovieGraphRenderDataIdentifier, TUniquePtr<FImagePixelData>>& RenderPassData : InRawFrameData->ImageOutputData)
	{
		constexpr int32 ResolutionIndex = 0;
		FMovieGraphResolveArgs ResolveArgs;
		const FString PreliminaryFileName = ResolveOutputFilename(InParentNode, InPipeline, ResolutionIndex, InRawFrameData, RenderPassData.Key, ResolveArgs);
		
		TArray<FMovieGraphRenderDataIdentifier>& RenderIDs = OutFilenameToRenderIDs.FindOrAdd(PreliminaryFileName);
		RenderIDs.Add(RenderPassData.Key);

		TArray<FIntPoint>& Resolutions = FilenameToResolutions.FindOrAdd(PreliminaryFileName);
		Resolutions.AddUnique(RenderPassData.Value->GetSize());

		MergeResolveArgs(ResolveArgs, OutFilenameToResolveArgs.FindOrAdd(PreliminaryFileName));
	}

	// Second, re-generate filenames if any render passes of differing resolutions map to the same file.
	for (const TPair<FString, TArray<FIntPoint>>& FilenameAndResolutions : FilenameToResolutions)
	{
		const FString& PreliminaryFilename = FilenameAndResolutions.Key;
		const TArray<FIntPoint>& Resolutions = FilenameAndResolutions.Value;

		// If there's only one resolution for this filename, there's nothing to disambiguate
		if (Resolutions.Num() == 1)
		{
			continue;
		}

		// If there IS more than one resolution for this filename, then disambiguate
		const TArray<FMovieGraphRenderDataIdentifier> OldRenderIDs = OutFilenameToRenderIDs.FindAndRemoveChecked(PreliminaryFilename);
		OutFilenameToResolveArgs.Remove(PreliminaryFilename);
		for (const FMovieGraphRenderDataIdentifier& RenderID : OldRenderIDs)
		{
			TUniquePtr<FImagePixelData>& ImageData = InRawFrameData->ImageOutputData[RenderID];
			const int32 ResolutionIndex = Resolutions.IndexOfByKey(ImageData->GetSize());

			// Re-resolve the filename, this time using the resolution index to generate a filename that will only contain
			// passes with this particular resolution
			FMovieGraphResolveArgs ResolveArgs;
			const FString FinalFilename = ResolveOutputFilename(InParentNode, InPipeline, ResolutionIndex, InRawFrameData, RenderID, ResolveArgs);

			TArray<FMovieGraphRenderDataIdentifier>& RenderIDs = OutFilenameToRenderIDs.FindOrAdd(FinalFilename);
			RenderIDs.Add(RenderID);

			MergeResolveArgs(ResolveArgs, OutFilenameToResolveArgs.FindOrAdd(FinalFilename));
		}
	}
}

FString UMovieGraphImageSequenceOutputNode_MultiLayerEXR::ResolveOutputFilename(
	const UMovieGraphImageSequenceOutputNode_MultiLayerEXR* InParentNode,
	const UMovieGraphPipeline* InPipeline,
	const int32 ResolutionIndex, const UE::MovieGraph::FMovieGraphOutputMergerFrame* InRawFrameData,
	const FMovieGraphRenderDataIdentifier& InRenderDataIdentifier, FMovieGraphResolveArgs& OutResolveArgs) const
{
	const TCHAR* Extension = TEXT("exr");

	constexpr bool bIncludeCDOs = true;
	const UMovieGraphGlobalOutputSettingNode* OutputSettings = InRawFrameData->EvaluatedConfig->GetSettingForBranch<UMovieGraphGlobalOutputSettingNode>(InRenderDataIdentifier.RootBranchName, bIncludeCDOs);
	if (!ensure(OutputSettings))
	{
		return FString();
	}
	
	// If we have more than one resolution we'll store it as "_Add" / "_Add(1)" etc via {ExtraTag}.
	FString FileNameFormatString = InParentNode->FileNameFormat + "{ExtraTag}";
	
	// If we're writing more than one render pass out, we need to ensure the file name has the format string in it so we don't
	// overwrite the same file multiple times. Burn In overlays don't count because they get composited on top of an existing file.
	constexpr bool bIncludeRenderPass = false;
	constexpr bool bTestFrameNumber = true;
	constexpr bool bIncludeCameraName = false;
	UE::MoviePipeline::ValidateOutputFormatString(FileNameFormatString, bIncludeRenderPass, bTestFrameNumber, bIncludeCameraName);
	
	// Create specific data that needs to override 
	TMap<FString, FString> FormatOverrides;
	FormatOverrides.Add(TEXT("render_pass"), TEXT("")); // Render Passes are included inside the exr file by named layers.
	FormatOverrides.Add(TEXT("ext"), Extension);

	// The logic for the ExtraTag is a little complicated. If there's only one layer (ideal situation) then it's empty.
	if (ResolutionIndex == 0)
	{
		FormatOverrides.Add(TEXT("ExtraTag"), TEXT(""));
	}
	else if (ResolutionIndex == 1)
	{
		// This is our most common case when we have a second file (the only expected one really)
		FormatOverrides.Add(TEXT("ExtraTag"), TEXT("_Add"));
	}
	else
	{
		// Finally a fallback in the event we have three or more unique resolutions.
		FormatOverrides.Add(TEXT("ExtraTag"), FString::Printf(TEXT("_Add(%d)"), ResolutionIndex));
	}

	// The layer's render data identifier is used here in the resolve. Usually this is not a problem. However, the user may include some tokens, like
	// {layer_name}, that come from the identifier, which will prevent all layers from being placed in the same multi-layer EXR (because now the path
	// isn't resolving to the path that other layers are resolving to). We have to assume that the user is doing this intentionally, even though it's
	// a bit strange. Including the full identifier here is important so all custom metadata is resolved correctly (see
	// UMovieGraphSetMetadataAttributesNode) when ResolveFilenameFormatArguments() is called.
	const FMovieGraphFilenameResolveParams Params = FMovieGraphFilenameResolveParams::MakeResolveParams(
		InRenderDataIdentifier, InPipeline, InRawFrameData->EvaluatedConfig.Get(), InRawFrameData->TraversalContext, FormatOverrides);
	
	const FString FilePathFormatString = OutputSettings->OutputDirectory.Path / FileNameFormatString;

	FString FinalFilePath = UMovieGraphBlueprintLibrary::ResolveFilenameFormatArguments(FilePathFormatString, Params, OutResolveArgs);

	if (FPaths::IsRelative(FinalFilePath))
	{
		FinalFilePath = FPaths::ConvertRelativePathToFull(FinalFilePath);
	}

	return FinalFilePath;
}
