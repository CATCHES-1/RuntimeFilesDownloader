// Georgy Treshchev 2024.

#pragma once

#include "BaseFilesDownloader.h"
#include "FileToStorageDownloader.generated.h"

/** Possible results from a download request */
UENUM(BlueprintType, Category = "File To Storage Downloader")
enum class EDownloadToStorageResult : uint8
{
	Success,
	/** Downloaded successfully, but there was no Content-Length header in the response and thus downloaded by payload */
	SucceededByPayload,
	/** Nothing was downloaded, since the provided If-None-Match header matched */
	NotModified,
	Cancelled,
	DownloadFailed,
	SaveFailed,
	DirectoryCreationFailed,
	InvalidURL,
	InvalidSavePath
};


/** Static delegate broadcast after the download is complete */
DECLARE_DELEGATE_ThreeParams(FOnFileToStorageDownloadCompleteNative, EDownloadToStorageResult, const FString&, const TArray<FString>&);

/** Dynamic delegate broadcast after the download is complete */
DECLARE_DYNAMIC_DELEGATE_TwoParams(FOnFileToStorageDownloadComplete, EDownloadToStorageResult, Result, const FString&, SavedPath);

enum class EDownloadToMemoryResult : uint8;

/**
 * Downloads a file and saves it to permanent storage
 */
UCLASS(BlueprintType, Category = "Runtime Files Downloader|Storage")
class RUNTIMEFILESDOWNLOADER_API UFileToStorageDownloader : public UBaseFilesDownloader
{
	GENERATED_BODY()

protected:
	/** Static delegate for monitoring the completion of the download */
	FOnFileToStorageDownloadCompleteNative OnDownloadComplete;

public:
	/**
	 * Download the file and save it to storage
	 *
	 * @param URL The URL of the file to be downloaded
	 * @param SavePath The absolute path and file name to save the downloaded file
	 * @param Timeout The maximum time to wait for the download to complete, in seconds. Works only for engine versions >= 4.26
	 * @param ContentType A string to set in the Content-Type header field. Use a MIME type to specify the file type
	 * @param bForceByPayload If true, download the file regardless of the Content-Length header's presence (useful for servers without support for this header)
	 * @param OnProgress Delegate for download progress updates
	 * @param OnComplete Delegate for broadcasting the completion of the download
	 * @note Headers are not supported since Blueprints have no TMap type.
	 */
	UFUNCTION(BlueprintCallable, Category = "Runtime Files Downloader|Storage")
	static UFileToStorageDownloader* DownloadFileToStorage(const FString& URL, const FString& SavePath, float Timeout, const FString& ContentType, bool bForceByPayload, const FOnDownloadProgress& OnProgress, const FOnFileToStorageDownloadComplete& OnComplete);

	/**
	 * Download the file and save it to storage. Suitable for use in C++
	 *
	 * @param URL The URL of the file to be downloaded
	 * @param SavePath The absolute path and file name to save the downloaded file
	 * @param Timeout The maximum time to wait for the download to complete, in seconds. Works only for engine versions >= 4.26
	 * @param ContentType A string to set in the Content-Type header field. Use a MIME type to specify the file type
	 * @param bForceByPayload If true, download the file regardless of the Content-Length header's presence (useful for servers without support for this header)
	 * @param OnProgress Delegate for download progress updates
	 * @param OnComplete Delegate for broadcasting the completion of the download
	 * @param Headers Additional headers to include in the request
	 */
	static UFileToStorageDownloader* DownloadFileToStorage(const FString& URL, const FString& SavePath, float Timeout, const FString& ContentType, bool bForceByPayload, const FOnDownloadProgressNative& OnProgress, const FOnFileToStorageDownloadCompleteNative& OnComplete, const TMap<FString, FString>& Headers = TMap<FString, FString>());

	//~ Begin UBaseFilesDownloader Interface
	virtual bool CancelDownload() override;
	//~ End UBaseFilesDownloader Interface

protected:
	/**
	 * Download the file and save it to the device disk
	 *
	 * @param URL The file URL to be downloaded
	 * @param SavePath The absolute path and file name to save the downloaded file
	 * @param Timeout The maximum time to wait for the download to complete, in seconds. Works only for engine versions >= 4.26
	 * @param ContentType A string to set in the Content-Type header field. Use a MIME type to specify the file type
	 * @param bForceByPayload If true, the file will be downloaded by payload even if the Content-Length header is present in the response
	 * @param Headers
	 */
	void DownloadFileToStorage(const FString& URL, const FString& SavePath, float Timeout, const FString& ContentType, bool bForceByPayload, const TMap<FString, FString>& Headers);

	/**
	 * Internal callback for when file downloading has finished
	 */
	void OnComplete_Internal(EDownloadToMemoryResult Result, TArray64<uint8> DownloadedContent, TArray<FString> Headers);

protected:
	/** The destination path to save the downloaded file */
	FString FileSavePath;
};
