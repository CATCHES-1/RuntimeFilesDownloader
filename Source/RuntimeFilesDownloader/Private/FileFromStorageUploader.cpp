#include "FileFromStorageUploader.h"

#include "FileToMemoryDownloader.h"
#include "RuntimeChunkDownloader.h"
#include "RuntimeFilesDownloaderDefines.h"
#include "Misc/Paths.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"

UFileFromStorageUploader* UFileFromStorageUploader::UploadFileFromStorage(
	const FString&                          URL, const FString& FilePath, float Timeout, const FOnDownloadProgress& OnProgress,
	const FOnFileFromStorageUploadComplete& OnComplete)
{
	return UploadFileFromStorage(URL, FilePath, Timeout, FOnDownloadProgressNative::CreateLambda(
		[OnProgress](int64 BytesReceived, int64 ContentSize, float ProgressRatio) {
			OnProgress.ExecuteIfBound(BytesReceived, ContentSize, ProgressRatio);
		}), FOnFileFromStorageUploadCompleteNative::CreateLambda(
		[OnComplete](EUploadFromStorageResult Result) {
			OnComplete.ExecuteIfBound(Result);
		}));
}

UFileFromStorageUploader* UFileFromStorageUploader::UploadFileFromStorage(
	const FString&                          URL, const FString& SavePath, float Timeout, const FOnDownloadProgressNative& OnProgress, const
	FOnFileFromStorageUploadCompleteNative& OnComplete)
{
	UFileFromStorageUploader* Uploader = NewObject<UFileFromStorageUploader>(StaticClass());
	Uploader->AddToRoot();
	Uploader->OnDownloadProgress = OnProgress;
	Uploader->OnUploadComplete = OnComplete;
	Uploader->UploadFileFromStorage(URL, SavePath, Timeout);
	return Uploader;
}

bool UFileFromStorageUploader::CancelDownload()
{
	if (RuntimeChunkDownloaderPtr.IsValid())
	{
		RuntimeChunkDownloaderPtr->CancelDownload();
		return true;
	}
	return false;
}

void UFileFromStorageUploader::UploadFileFromStorage(const FString& URL, const FString& SourceFile, float Timeout)
{
	if (URL.IsEmpty())
	{
		UE_LOG(LogRuntimeFilesDownloader, Error, TEXT("You have not provided an URL to upload the file"));
		OnUploadComplete.ExecuteIfBound(EUploadFromStorageResult::InvalidURL);
		RemoveFromRoot();
		return;
	}

	if (SourceFile.IsEmpty())
	{
		UE_LOG(LogRuntimeFilesDownloader, Error, TEXT("You have not provided a path for the file to be uploaded"));
		OnUploadComplete.ExecuteIfBound(EUploadFromStorageResult::InvalidPath);
		RemoveFromRoot();
		return;
	}

	if (Timeout < 0)
	{
		UE_LOG(LogRuntimeFilesDownloader, Warning, TEXT("The specified timeout (%f) is less than 0, setting it to 0"),
			Timeout);
		Timeout = 0;
	}

	auto OnProgress = [this](int64 BytesReceived, int64 ContentSize) {
		BroadcastProgress(BytesReceived, ContentSize,
			ContentSize <= 0 ? 0 : static_cast<float>(BytesReceived) / ContentSize);
	};

	auto OnResult = [this](FRuntimeChunkUploaderResult&& Result) mutable {
		OnComplete_Internal(Result.Result);
	};

	// Read the file from disk
	TArray<uint8> Body;
	if (!FFileHelper::LoadFileToArray(Body, *SourceFile))
	{
		UE_LOG(LogRuntimeFilesDownloader, Error,
			TEXT("Something went wrong while reading the file '%s'"), *SourceFile);
		#if PLATFORM_LINUX || PLATFORM_MAC // Notably, PLATFORM_UNIX is not set on macOS (whyyy?)
		UE_LOG(LogInit, Warning, TEXT("Failed to read file with errno: %d: %s"),
			errno, UTF8_TO_TCHAR(strerror(errno)));
		#elif PLATFORM_WINDOWS
		// Figure it out yourself. Sorry.
		#endif
		OnUploadComplete.ExecuteIfBound(EUploadFromStorageResult::LoadFailed);
		RemoveFromRoot();
		return;
	}

	RuntimeChunkDownloaderPtr = MakeShared<FRuntimeChunkDownloader>();
	RuntimeChunkDownloaderPtr->UploadFile(URL, Timeout, Body, OnProgress).Next(OnResult);
}

void UFileFromStorageUploader::OnComplete_Internal(EUploadFromStorageResult Result)
{
	RemoveFromRoot();

	EUploadFromStorageResult ResultUpstream;
	switch (Result)
	{
		case EUploadFromStorageResult::Cancelled:
			ResultUpstream = EUploadFromStorageResult::Cancelled;
			break;
		case EUploadFromStorageResult::InvalidPath:
			ResultUpstream = EUploadFromStorageResult::InvalidPath;
			break;
		case EUploadFromStorageResult::InvalidURL:
			ResultUpstream = EUploadFromStorageResult::InvalidURL;
			break;
		case EUploadFromStorageResult::LoadFailed:
			ResultUpstream = EUploadFromStorageResult::LoadFailed;
			break;
		case EUploadFromStorageResult::UploadFailed:
			ResultUpstream = EUploadFromStorageResult::UploadFailed;
			break;
		case EUploadFromStorageResult::Success:
			ResultUpstream = EUploadFromStorageResult::Success;
			break;
		default:
			UE_LOG(LogRuntimeFilesDownloader, Error, TEXT("Unknown upload result: %d %s"), static_cast<int32>(Result),
				*UEnum::GetValueAsString(Result));
			return;
	}
	OnUploadComplete.ExecuteIfBound(ResultUpstream);
}