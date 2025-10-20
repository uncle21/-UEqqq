// Copyright Epic Games, Inc. All Rights Reserved.

#include "MoviePipelineImageSequenceOutput.h"
#include "ImageWriteTask.h"
#include "ImagePixelData.h"
#include "Modules/ModuleManager.h"
#include "ImageWriteQueue.h"
#include "MoviePipeline.h"
#include "ImageWriteStream.h"
#include "MoviePipelinePrimaryConfig.h"
#include "MovieRenderTileImage.h"
#include "MovieRenderOverlappedImage.h"
#include "MovieRenderPipelineCoreModule.h"
#include "Misc/FrameRate.h"
#include "MoviePipelineOutputSetting.h"
#include "MoviePipelineBurnInSetting.h"
#include "Containers/UnrealString.h"
#include "Misc/StringFormatArg.h"
#include "MoviePipelineOutputBase.h"
#include "MoviePipelineImageQuantization.h"
#include "MoviePipelineWidgetRenderSetting.h"
#include "MoviePipelineUtils.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(MoviePipelineImageSequenceOutput)

DECLARE_CYCLE_STAT(TEXT("ImgSeqOutput_RecieveImageData"), STAT_ImgSeqRecieveImageData, STATGROUP_MoviePipeline);

namespace UE
{
	namespace MoviePipeline
	{
		FAsyncImageQuantization::FAsyncImageQuantization(FImageWriteTask* InWriteTask, const bool bInConvertToSRGB)
			: ParentWriteTask(InWriteTask)
			, bConvertToSRGB(bInConvertToSRGB)
		{
		}

		void FAsyncImageQuantization::operator()(FImagePixelData* PixelData)
		{
			// Note: Ideally we would use FImageCore routines here, but there is no easy way to construct pixel data from an FImage currently.

			// Convert the incoming data to 8-bit, potentially with sRGB applied.
			TUniquePtr<FImagePixelData> QuantizedPixelData = QuantizeImagePixelDataToBitDepth(PixelData, 8, nullptr, bConvertToSRGB);
			ParentWriteTask->PixelData = MoveTemp(QuantizedPixelData);
		}
	}
}

UMoviePipelineImageSequenceOutputBase::UMoviePipelineImageSequenceOutputBase()
{
	if (!HasAnyFlags(RF_ArchetypeObject))
	{
		ImageWriteQueue = &FModuleManager::Get().LoadModuleChecked<IImageWriteQueueModule>("ImageWriteQueue").GetWriteQueue();
	}
}

void UMoviePipelineImageSequenceOutputBase::BeginFinalizeImpl()
{
	FinalizeFence = ImageWriteQueue->CreateFence();
}

bool UMoviePipelineImageSequenceOutputBase::HasFinishedProcessingImpl()
{ 
	// Wait until the finalization fence is reached meaning we've written everything to disk.
	return Super::HasFinishedProcessingImpl() && (!FinalizeFence.IsValid() || FinalizeFence.WaitFor(0));
}

void UMoviePipelineImageSequenceOutputBase::OnShotFinishedImpl(const UMoviePipelineExecutorShot* InShot, const bool bFlushToDisk)
{
	if (bFlushToDisk)
	{
		UE_LOG(LogMovieRenderPipelineIO, Log, TEXT("ImageSequenceOutputBase flushing %d tasks to disk, inserting a fence in the queue and then waiting..."), ImageWriteQueue->GetNumPendingTasks());
		const double FlushBeginTime = FPlatformTime::Seconds();

		TFuture<void> Fence = ImageWriteQueue->CreateFence();
		Fence.Wait();
		const float ElapsedS = float((FPlatformTime::Seconds() - FlushBeginTime));
		UE_LOG(LogMovieRenderPipelineIO, Log, TEXT("Finished flushing tasks to disk after %2.2fs!"), ElapsedS);
	}
}

void UMoviePipelineImageSequenceOutputBase::OnReceiveImageDataImpl(FMoviePipelineMergerOutputFrame* InMergedOutputFrame)
{
	SCOPE_CYCLE_COUNTER(STAT_ImgSeqRecieveImageData);

	check(InMergedOutputFrame);

	// Special case for extracting Burn Ins and Widget Renderer 
	TArray<MoviePipeline::FCompositePassInfo> CompositedPasses;
	MoviePipeline::GetPassCompositeData(InMergedOutputFrame, CompositedPasses);


	UMoviePipelineOutputSetting* OutputSettings = GetPipeline()->GetPipelinePrimaryConfig()->FindSetting<UMoviePipelineOutputSetting>();
	check(OutputSettings);

	UMoviePipelineColorSetting* ColorSetting = GetPipeline()->GetPipelinePrimaryConfig()->FindSetting<UMoviePipelineColorSetting>();

	FString OutputDirectory = OutputSettings->OutputDirectory.Path;

	// The InMergedOutputFrame->ImageOutputData map contains both RenderPasses and CompositePasses.
	// We determine how we gather pixel data based on the number of RenderPasses we have done, not counting the CompositePasses.
	// This is the reason for using a separate RenderPassIteration counter with a foreach loop, only incrementing it for RenderPasses.
	int32 RenderPassIteration = 0;
	const int32 RenderPassCount = InMergedOutputFrame->ImageOutputData.Num() - CompositedPasses.Num();
	for (TPair<FMoviePipelinePassIdentifier, TUniquePtr<FImagePixelData>>& RenderPassData : InMergedOutputFrame->ImageOutputData)
	{
		// Don't write out a composited pass in this loop, as it will be merged with the Final Image and not written separately. 
		bool bSkip = false;
		for (const MoviePipeline::FCompositePassInfo& CompositePass : CompositedPasses)
		{
			if (CompositePass.PassIdentifier == RenderPassData.Key)
			{
				bSkip = true;
				break;
			}
		}

		if (bSkip)
		{
			continue;
		}

		//11123
		const FString DepthPassName = TEXT("BitPatternDepth");

		if (RenderPassData.Key.Name == DepthPassName)
		{
			const EImagePixelType PixelType = RenderPassData.Value->GetType();

			// 假设 AOV 输出是 32-bit 浮点数 (R32F)
			if (PixelType == EImagePixelType::Float16 || PixelType == EImagePixelType::Float16)
			{
				// 1. 修正: 向下转型到 TImagePixelData<float> 以访问像素数组
				const TImagePixelData<float>* FloatPixelData =
					static_cast<const TImagePixelData<float>*>(RenderPassData.Value.Get());

				if (FloatPixelData)
				{
					const TArray64<float>& RawFloatData = FloatPixelData->Pixels;
					const float* FloatPtr = RawFloatData.GetData();
					int32 NumPixels = RawFloatData.Num();

					TArray<uint16> Uint16BitPattern;
					Uint16BitPattern.SetNumUninitialized(NumPixels);

					// 2. 核心转换: Normalized Float (0-1) -> Uint16 Bit Pattern (0-65535)
					for (int32 i = 0; i < NumPixels; ++i)
					{
						float NormalizedValue = FloatPtr[i];

						// 四舍五入到最近的整数，以恢复原始的 Uint16Depth 位模式
						uint32 RawUint16 = FMath::RoundToInt(NormalizedValue * 65535.0f);

						// 钳位到 16-bit 范围 [0, 65535] 并转换为 uint16
						Uint16BitPattern[i] = (uint16)FMath::Clamp(RawUint16, 0u, 65535u);
					}

					// 3. 配置 Image Write Task
					TUniquePtr<FImageWriteTask> WriteTask = MakeUnique<FImageWriteTask>();
					WriteTask->Format = EImageFormat::PNG;

					// --- 文件名修正: 使用 GetFormatArguments 辅助函数 ---
					FMoviePipelineFormatArgs Args;
					GetFormatArguments(Args); // 这个函数存在于 UMoviePipelineImageSequenceOutputBase 基类中

					// RenderPassData.Value->GetFilename() 函数在 FImagePixelData 中可能不存在
					// 我们使用 RenderPassData.Key.Name 来确保文件名中包含 AOV 名称

					// 获取文件名的格式字符串 (例如: {shot_name}.{frame_number}.{render_pass})
					FString FileNameFormatString = OutputDirectory / OutputSettings->FileNameFormat; // 从 UMoviePipelineOutputBase 继承

					// 添加 RenderPass 占位符
					FString PassIdentifier = FString::Printf(TEXT("%s"), *RenderPassData.Key.Name);
					FileNameFormatString = FileNameFormatString.Replace(TEXT("{render_pass}"), *PassIdentifier);

					// 解析最终文件名
					FString FinalFilename = FString::Format(*FormatString, Args.Arguments);

					// 4. 设置 ImageWriteTask 的文件名
					WriteTask->Filename = FinalFilename;

					// 4. 设置 ImageWriteTask 的文件名
					WriteTask->Filename = FinalFilename;
					// --- 文件名修正结束 ---


					// 设置像素布局为 R16_UINT (单通道 16-bit 无符号整数, G16 格式)
					// 这是确保 PNG 写入 16-bit 整数的关键
					WriteTask->PixelLayout = FImagePixelDataLayout(
						RenderPassData.Value->GetDimensions(),
						EImagePixelType::UInt16,
						ERawImageFormat::G16 // G16 for 16-bit Grayscale (16-bit 1-channel unsigned integer)
					);

					// 5. 创建包含我们转换后的 uint16 数据的新像素数据对象
					TUniquePtr<TImagePixelData<uint16>> NewPixelData = MakeUnique<TImagePixelData<uint16>>(
						RenderPassData.Value->GetDimensions(),
						TArray64<uint16>(MoveTemp(Uint16BitPattern)), // 将数据移动到 FImageWriteTask 队列
						EImagePixelType::UInt16,
						ERawImageFormat::G16,
						RenderPassData.Value->GetColorSpace(),
						RenderPassData.Value->GetPayload()
					);

					WriteTask->InputData = MoveTemp(NewPixelData);

					// 6. 提交写入任务到队列
					ImageWriteQueue->Enqueue(MoveTemp(WriteTask));

					// 7. 跳过 base class 对此 Pass 的后续默认处理
					continue;
				}
				else
				{
					UE_LOG(LogMovieRenderPipeline, Error, TEXT("Depth Bit Pattern AOV ('%s') failed to cast to TImagePixelData<float>. Check if your AOV is configured for single-channel R32F."), *DepthPassName);
				}
			}
			else
			{
				UE_LOG(LogMovieRenderPipeline, Warning, TEXT("Depth Bit Pattern AOV ('%s') is not the expected 32-bit Float format. Skipping bit pattern conversion. Actual Type: %d"),
					*DepthPassName, (int32)PixelType);
			}
		}


		EImageFormat PreferredOutputFormat = OutputFormat;

		FImagePixelDataPayload* Payload = RenderPassData.Value->GetPayload<FImagePixelDataPayload>();

		// If the output requires a transparent output (to be useful) then we'll on a per-case basis override their intended
		// filetype to something that makes that file useful.
		if (Payload->bRequireTransparentOutput)
		{
			if (PreferredOutputFormat == EImageFormat::BMP ||
				PreferredOutputFormat == EImageFormat::JPEG)
			{
				PreferredOutputFormat = EImageFormat::PNG;
			}
		}

		const TCHAR* Extension = TEXT("");
		switch (PreferredOutputFormat)
		{
		case EImageFormat::PNG: Extension = TEXT("png"); break;
		case EImageFormat::JPEG: Extension = TEXT("jpeg"); break;
		case EImageFormat::BMP: Extension = TEXT("bmp"); break;
		case EImageFormat::EXR: Extension = TEXT("exr"); break;
		}


		// We need to resolve the filename format string. We combine the folder and file name into one long string first
		MoviePipeline::FMoviePipelineOutputFutureData OutputData;
		OutputData.Shot = GetPipeline()->GetActiveShotList()[Payload->SampleState.OutputState.ShotIndex];
		OutputData.PassIdentifier = RenderPassData.Key;

		struct FXMLData
		{
			FString ClipName;
			FString ImageSequenceFileName;
		};
		
		FXMLData XMLData;
		{
			FString FileNameFormatString = OutputDirectory / OutputSettings->FileNameFormat;

			// If we're writing more than one render pass out, we need to ensure the file name has the format string in it so we don't
			// overwrite the same file multiple times. Burn In overlays don't count if they are getting composited on top of an existing file.
			const bool bIncludeRenderPass = InMergedOutputFrame->HasDataFromMultipleRenderPasses(CompositedPasses);
			const bool bIncludeCameraName = InMergedOutputFrame->HasDataFromMultipleCameras();
			const bool bTestFrameNumber = true;

			UE::MoviePipeline::ValidateOutputFormatString(/*InOut*/ FileNameFormatString, bIncludeRenderPass, bTestFrameNumber, bIncludeCameraName);

			// Create specific data that needs to override 
			TMap<FString, FString> FormatOverrides;
			FormatOverrides.Add(TEXT("render_pass"), RenderPassData.Key.Name);
			FormatOverrides.Add(TEXT("ext"), Extension);
			FMoviePipelineFormatArgs FinalFormatArgs;

			// Resolve for XMLs
			{
				GetPipeline()->ResolveFilenameFormatArguments(/*In*/ FileNameFormatString, FormatOverrides, /*Out*/ XMLData.ImageSequenceFileName, FinalFormatArgs, &Payload->SampleState.OutputState, -Payload->SampleState.OutputState.ShotOutputFrameNumber);
			}
			
			// Resolve the final absolute file path to write this to
			{
				GetPipeline()->ResolveFilenameFormatArguments(FileNameFormatString, FormatOverrides, OutputData.FilePath, FinalFormatArgs, &Payload->SampleState.OutputState);

				if (FPaths::IsRelative(OutputData.FilePath))
				{
					OutputData.FilePath = FPaths::ConvertRelativePathToFull(OutputData.FilePath);
				}
			}

			// More XML resolving. Create a deterministic clipname by removing frame numbers, file extension, and any trailing .'s
			{
				UE::MoviePipeline::RemoveFrameNumberFormatStrings(FileNameFormatString, true);
				GetPipeline()->ResolveFilenameFormatArguments(FileNameFormatString, FormatOverrides, XMLData.ClipName, FinalFormatArgs, &Payload->SampleState.OutputState);
				XMLData.ClipName.RemoveFromEnd(Extension);
				XMLData.ClipName.RemoveFromEnd(".");
			}
		}

		TUniquePtr<FImageWriteTask> TileImageTask = MakeUnique<FImageWriteTask>();
		TileImageTask->Format = PreferredOutputFormat;
		TileImageTask->CompressionQuality = 100;
		TileImageTask->Filename = OutputData.FilePath;

		TUniquePtr<FImagePixelData> QuantizedPixelData = RenderPassData.Value->CopyImageData();
		EImagePixelType QuantizedPixelType = QuantizedPixelData->GetType();

		switch (PreferredOutputFormat)
		{
		case EImageFormat::PNG:
		case EImageFormat::JPEG:
		case EImageFormat::BMP:
		{
			// All three of these formats only support 8 bit data, so we need to take the incoming buffer type,
			// copy it into a new 8-bit array and apply a little noise to the data to help hide gradient banding.
			const bool bApplysRGB = !(ColorSetting && ColorSetting->OCIOConfiguration.bIsEnabled);
			TileImageTask->PixelPreProcessors.Add(UE::MoviePipeline::FAsyncImageQuantization(TileImageTask.Get(), bApplysRGB));

			// The pixel type will get changed by this pre-processor so future calculations below need to know the correct type they'll be editing.
			QuantizedPixelType = EImagePixelType::Color; 
			break;
		}
		case EImageFormat::EXR:
			// No quantization required, just copy the data as we will move it into the image write task.
			break;
		default:
			check(false);
		}


		// We composite before flipping the alpha so that it is consistent for all formats.
		if (RenderPassData.Key.Name == TEXT("FinalImage") || RenderPassData.Key.Name == TEXT("PathTracer")) 
		{
			for (const MoviePipeline::FCompositePassInfo& CompositePass : CompositedPasses)
			{
				// Match them up by camera name so multiple passes intended for different camera names work.
				if (RenderPassData.Key.CameraName != CompositePass.PassIdentifier.CameraName)
				{
					continue;
				}

				// Check that the composite resolution matches the camera resolution to ensure the composite pass doesn't fail.
				// This can happen if multiple cameras with different amounts of overscan are rendered, since composite passes
				// don't support rendering at multiple resolutions
				const FIntPoint CompositeResolution = CompositePass.PixelData->GetSize();
				const FIntPoint CameraResolution = RenderPassData.Value->GetSize();
				if (CompositeResolution != CameraResolution)
				{
					UE_LOG(LogMovieRenderPipeline, Warning, TEXT("Composite resolution %dx%d does not match camera resolution %dx%d, skipping composite for %s on camera %s"),
						CompositeResolution.X, CompositeResolution.Y,
						CameraResolution.X, CameraResolution.Y,
						*CompositePass.PassIdentifier.Name,
						*RenderPassData.Key.CameraName);

					continue;
				}

				// If there's more than one render pass, we need to copy the composite passes for the first render pass then move for the remaining ones
				const bool bShouldCopyImageData = RenderPassCount > 1 && RenderPassIteration == 0;
				TUniquePtr<FImagePixelData> PixelData = bShouldCopyImageData ? CompositePass.PixelData->CopyImageData() : CompositePass.PixelData->MoveImageDataToNew();
				
				// We don't need to copy the data here (even though it's being passed to a async system) because we already made a unique copy of the
				// burn in/widget data when we decided to composite it.
				switch (QuantizedPixelType)
				{
					case EImagePixelType::Color:
						TileImageTask->PixelPreProcessors.Add(TAsyncCompositeImage<FColor>(MoveTemp(PixelData)));
						break;
					case EImagePixelType::Float16:
						TileImageTask->PixelPreProcessors.Add(TAsyncCompositeImage<FFloat16Color>(MoveTemp(PixelData)));
						break;
					case EImagePixelType::Float32:
						TileImageTask->PixelPreProcessors.Add(TAsyncCompositeImage<FLinearColor>(MoveTemp(PixelData)));
						break;
				}
			}
		}

		// A payload _requiring_ alpha output will override the Write Alpha option, because that flag is used to indicate that the output is
		// no good without alpha, and we already did logic above to ensure it got turned into a filetype that could write alpha.
		if (!IsAlphaAllowed() && !Payload->bRequireTransparentOutput)
		{
			TileImageTask->AddPreProcessorToSetAlphaOpaque();
		}


		TileImageTask->PixelData = MoveTemp(QuantizedPixelData);
		
#if WITH_EDITOR
		GetPipeline()->AddFrameToOutputMetadata(XMLData.ClipName, XMLData.ImageSequenceFileName, Payload->SampleState.OutputState, Extension, Payload->bRequireTransparentOutput);
#endif

		GetPipeline()->AddOutputFuture(ImageWriteQueue->Enqueue(MoveTemp(TileImageTask)), OutputData);

		RenderPassIteration++;
	}
}


void UMoviePipelineImageSequenceOutputBase::GetFormatArguments(FMoviePipelineFormatArgs& InOutFormatArgs) const
{
	// Stub in a dummy extension (so people know it exists)
	// InOutFormatArgs.Arguments.Add(TEXT("ext"), TEXT("jpg/png/exr")); Hidden since we just always post-pend with an extension.
	InOutFormatArgs.FilenameArguments.Add(TEXT("render_pass"), TEXT("RenderPassName"));
}


