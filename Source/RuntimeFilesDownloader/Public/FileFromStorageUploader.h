#pragma once

#include "BaseFilesDownloader.h"
#include "FileFromStorageUploader.generated.h"

/** Possible results from an upload request */
UENUM(BlueprintType, Category = "File From Storage Uploader")
enum class EUploadFromStorageResult : uint8
{
	Success,
	Cancelled,
	UploadFailed,
	LoadFailed,
	InvalidURL,
	InvalidPath
};


/** Static delegate broadcast after the upload is complete */
DECLARE_DELEGATE_OneParam(FOnFileFromStorageUploadCompleteNative, EUploadFromStorageResult);

/** Dynamic delegate broadcast after the upload is complete */
DECLARE_DYNAMIC_DELEGATE_OneParam(FOnFileFromStorageUploadComplete, EUploadFromStorageResult, Result);

enum class EDownloadToMemoryResult : uint8;

/**
 * Uploads a file loaded from a path
 */
UCLASS(BlueprintType, Category = "Runtime Files Downloader|Storage")
class RUNTIMEFILESDOWNLOADER_API UFileFromStorageUploader : public UBaseFilesDownloader
{
	GENERATED_BODY()

protected:
	/** Static delegate for monitoring the completion of the upload */
	FOnFileFromStorageUploadCompleteNative OnUploadComplete;

public:
	/**
	 * Upload a file from Storage to the specified URL. Suitable for use in Blueprints
	 *
	 * @param URL The URL to upload the file
	 * @param FilePath The absolute path and file name to load the file from
	 * @param Timeout The maximum time to wait for the upload to complete, in seconds.
	 * @param OnProgress Delegate for upload progress updates
	 * @param OnComplete Delegate for broadcasting the completion of the upload
	 */
	UFUNCTION(BlueprintCallable, Category = "Runtime Files Downloader|Storage")
	static UFileFromStorageUploader* UploadFileFromStorage(const FString& URL, const FString& FilePath, float Timeout,
														   const FOnDownloadProgress& OnProgress,
														   const FOnFileFromStorageUploadComplete& OnComplete);

	/**
	 * Upload a file from Storage to the specified URL. Suitable for use in C++
	 *
	 * @param URL The URL to upload the file
	 * @param SavePath The absolute path and file name to load the file from
	 * @param Timeout The maximum time to wait for the upload to complete, in seconds.
	 * @param OnProgress Delegate for upload progress updates
	 * @param OnComplete Delegate for broadcasting the completion of the upload
	 * @param Headers Additional request headers to include in the request
	 */
	static UFileFromStorageUploader* UploadFileFromStorage(const FString& URL, const FString& SavePath, float Timeout,
														   const FOnDownloadProgressNative& OnProgress, const
														   FOnFileFromStorageUploadCompleteNative& OnComplete,
														   const TMap<FString, FString>& Headers = TMap<FString, FString>());

	//~ Begin UBaseFilesDownloader Interface
	virtual bool CancelDownload() override;
	//~ End UBaseFilesDownloader Interface

protected:
	/**
	 * Upload the file from the specified path
	 *
	 * @param URL The URL for the file to be uploaded to
	 * @param FilePath The absolute path and file name to load the file from
	 * @param Timeout The maximum time to wait for the upload to complete, in seconds.
	 * @param Headers Additional request headers to include in the request
	 */
	void UploadFileFromStorage(const FString& URL, const FString& FilePath, float Timeout, const TMap<FString, FString>& Headers = TMap<FString, FString>());

	/**
	 * Internal callback for when file uploading has finished
	 */
	void OnComplete_Internal(EUploadFromStorageResult Result);
};
