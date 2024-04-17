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
	 */
	static UFileFromStorageUploader* UploadFileFromStorage(const FString& URL, const FString& SavePath, float Timeout,
														   const FOnDownloadProgressNative& OnProgress, const
														   FOnFileFromStorageUploadCompleteNative& OnComplete);

	//~ Begin UBaseFilesDownloader Interface
	virtual bool CancelDownload() override;
	//~ End UBaseFilesDownloader Interface

protected:
	/**
	 * Download the file and save it to the device disk
	 *
	 * @param URL The file URL to be uploaded
	 * @param FilePath The absolute path and file name to load the file from
	 * @param Timeout The maximum time to wait for the upload to complete, in seconds.
	 */
	void UploadFileFromStorage(const FString& URL, const FString& FilePath, float Timeout);

	/**
	 * Internal callback for when file uploading has finished
	 */
	void OnComplete_Internal(EUploadFromStorageResult Result);
};