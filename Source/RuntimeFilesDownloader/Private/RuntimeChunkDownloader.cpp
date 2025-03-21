﻿// Georgy Treshchev 2024.

#include "RuntimeChunkDownloader.h"

#include "FileFromStorageUploader.h"
#include "FileToMemoryDownloader.h"
#include "RuntimeFilesDownloaderDefines.h"

FRuntimeChunkDownloader::FRuntimeChunkDownloader()
	: bCanceled(false)
{}

FRuntimeChunkDownloader::~FRuntimeChunkDownloader()
{
	UE_LOG(LogRuntimeFilesDownloader, Warning, TEXT("FRuntimeChunkDownloader destroyed"));
}

TFuture<FRuntimeChunkDownloaderResult> FRuntimeChunkDownloader::DownloadFile(const FString& URL, float Timeout, const FString& ContentType, int64 MaxChunkSize, const TFunction<void(int64, int64)>& OnProgress, const TMap<FString, FString>& Headers)
{
	if (bCanceled)
	{
		UE_LOG(LogRuntimeFilesDownloader, Warning, TEXT("Canceled file download from %s"), *URL);
		return MakeFulfilledPromise<FRuntimeChunkDownloaderResult>(FRuntimeChunkDownloaderResult{EDownloadToMemoryResult::Cancelled, {}, {}}).GetFuture();
	}

	TSharedPtr<TPromise<FRuntimeChunkDownloaderResult>> PromisePtr = MakeShared<TPromise<FRuntimeChunkDownloaderResult>>();
	TWeakPtr<FRuntimeChunkDownloader> WeakThisPtr = AsShared();
	GetContentSize(URL, Timeout, Headers).Next([WeakThisPtr, PromisePtr, URL, Timeout, ContentType, MaxChunkSize, OnProgress, Headers](int64 ContentSize) mutable
	{
		TSharedPtr<FRuntimeChunkDownloader> SharedThis = WeakThisPtr.Pin();
		if (!SharedThis.IsValid())
		{
			UE_LOG(LogRuntimeFilesDownloader, Warning, TEXT("Failed to download file chunk from %s: downloader has been destroyed"), *URL);
			PromisePtr->SetValue(FRuntimeChunkDownloaderResult{EDownloadToMemoryResult::DownloadFailed, {}, {}});
			return;
		}

		if (SharedThis->bCanceled)
		{
			UE_LOG(LogRuntimeFilesDownloader, Warning, TEXT("Canceled file download from %s"), *URL);
			PromisePtr->SetValue(FRuntimeChunkDownloaderResult{EDownloadToMemoryResult::Cancelled, {}, {}});
			return;
		}

		auto DownloadByPayload = [SharedThis, WeakThisPtr, PromisePtr, URL, Timeout, ContentType, OnProgress, Headers]()
		{
			SharedThis->DownloadFileByPayload(URL, Timeout, ContentType, OnProgress, Headers).Next([WeakThisPtr, PromisePtr, URL, Timeout, ContentType, OnProgress](FRuntimeChunkDownloaderResult Result) mutable
			{
				TSharedPtr<FRuntimeChunkDownloader> SharedThis = WeakThisPtr.Pin();
				if (!SharedThis.IsValid())
					{
					UE_LOG(LogRuntimeFilesDownloader, Warning, TEXT("Failed to download file chunk from %s: downloader has been destroyed"), *URL);
					PromisePtr->SetValue(FRuntimeChunkDownloaderResult{EDownloadToMemoryResult::DownloadFailed, {}, {}});
					return;
				}

				if (SharedThis->bCanceled)
				{
					UE_LOG(LogRuntimeFilesDownloader, Warning, TEXT("Canceled file chunk download from %s"), *URL);
					PromisePtr->SetValue(FRuntimeChunkDownloaderResult{EDownloadToMemoryResult::Cancelled, {}, {}});
					return;
				}

				PromisePtr->SetValue(FRuntimeChunkDownloaderResult{Result.Result, MoveTemp(Result.Data)});
			});
		};

		// -304 is used by GetContentSize to signal that the HEAD request returned a "304 Not Modified" instead of a size.
		if (ContentSize == -304)
		{
			PromisePtr->SetValue(FRuntimeChunkDownloaderResult{EDownloadToMemoryResult::NotModified, {}, {}});
			return;
		}
		if (ContentSize <= 0)
		{
			UE_LOG(LogRuntimeFilesDownloader, Warning, TEXT("Unable to get content size for %s. Trying to download the file by payload"), *URL);
			DownloadByPayload();
			return;
		}

		if (MaxChunkSize <= 0)
		{
			UE_LOG(LogRuntimeFilesDownloader, Error, TEXT("Failed to download file chunk from %s: MaxChunkSize is <= 0. Trying to download the file by payload"), *URL);
			DownloadByPayload();
			return;
		}

		TSharedPtr<TArray64<uint8>> OverallDownloadedDataPtr = MakeShared<TArray64<uint8>>();
		{
			UE_LOG(LogRuntimeFilesDownloader, Warning, TEXT("Pre-allocating %lld bytes for file download from %s"), ContentSize, *URL);
			OverallDownloadedDataPtr->SetNumUninitialized(ContentSize);
		}

		FInt64Vector2 ChunkRange;
		{
			ChunkRange.X = 0;
			ChunkRange.Y = FMath::Min(MaxChunkSize, ContentSize) - 1;
		}

		TSharedPtr<int64> ChunkOffsetPtr = MakeShared<int64>(ChunkRange.X);
		TSharedPtr<bool> bChunkDownloadedFilledPtr = MakeShared<bool>(false);

		auto OnChunkDownloadedFilled = [bChunkDownloadedFilledPtr]()
		{
			if (bChunkDownloadedFilledPtr.IsValid())
			{
				*bChunkDownloadedFilledPtr = true;
			}
		};

		auto OnChunkDownloaded = [WeakThisPtr, PromisePtr, URL, ContentSize, Timeout, ContentType, OnProgress, DownloadByPayload, OverallDownloadedDataPtr, bChunkDownloadedFilledPtr, ChunkOffsetPtr, OnChunkDownloadedFilled](TArray64<uint8>&& ResultData) mutable
		{
			TSharedPtr<FRuntimeChunkDownloader> SharedThis = WeakThisPtr.Pin();
			if (!SharedThis.IsValid())
			{
				UE_LOG(LogRuntimeFilesDownloader, Warning, TEXT("Failed to download file chunk from %s: downloader has been destroyed"), *URL);
				PromisePtr->SetValue(FRuntimeChunkDownloaderResult{EDownloadToMemoryResult::DownloadFailed, {}, {}});
				OnChunkDownloadedFilled();
				return;
			}

			if (SharedThis->bCanceled)
			{
				UE_LOG(LogRuntimeFilesDownloader, Warning, TEXT("Canceled file chunk download from %s"), *URL);
				PromisePtr->SetValue(FRuntimeChunkDownloaderResult{EDownloadToMemoryResult::Cancelled, {}, {}});
				OnChunkDownloadedFilled();
				return;
			}

			// Calculate the currently size of the downloaded content in the result buffer
			const int64 CurrentlyDownloadedSize = *ChunkOffsetPtr + ResultData.Num();

			// Check if some values are out of range
			{
				if (*ChunkOffsetPtr < 0 || *ChunkOffsetPtr >= OverallDownloadedDataPtr->Num())
				{
					UE_LOG(LogRuntimeFilesDownloader, Error, TEXT("Failed to download file chunk from %s: data offset is out of range (%lld, expected [0, %lld]). Trying to download the file by payload"), *URL, *ChunkOffsetPtr, OverallDownloadedDataPtr->Num());
					DownloadByPayload();
					OnChunkDownloadedFilled();
					return;
				}

				if (CurrentlyDownloadedSize > OverallDownloadedDataPtr->Num())
				{
					UE_LOG(LogRuntimeFilesDownloader, Error, TEXT("Failed to download file chunk from %s: overall downloaded size is out of range (%lld, expected [0, %lld]). Trying to download the file by payload"), *URL, CurrentlyDownloadedSize, OverallDownloadedDataPtr->Num());
					DownloadByPayload();
					OnChunkDownloadedFilled();
					return;
				}
			}

			// Append the downloaded chunk to the result data
			FMemory::Memcpy(OverallDownloadedDataPtr->GetData() + *ChunkOffsetPtr, ResultData.GetData(), ResultData.Num());

			// If the download is complete, return the result data
			if (*ChunkOffsetPtr + ResultData.Num() >= ContentSize)
			{
				PromisePtr->SetValue(FRuntimeChunkDownloaderResult{EDownloadToMemoryResult::Success, MoveTemp(*OverallDownloadedDataPtr.Get())});
				OnChunkDownloadedFilled();
				return;
			}

			// Increase the offset by the size of the downloaded chunk
			*ChunkOffsetPtr += ResultData.Num();
		};

		SharedThis->DownloadFilePerChunk(URL, Timeout, ContentType, MaxChunkSize, ChunkRange, OnProgress, OnChunkDownloaded, Headers).Next([PromisePtr, bChunkDownloadedFilledPtr, URL, OverallDownloadedDataPtr, OnChunkDownloadedFilled, DownloadByPayload](EDownloadToMemoryResult Result) mutable
		{
			// Only return data if no chunk was downloaded
			if (bChunkDownloadedFilledPtr.IsValid() && (*bChunkDownloadedFilledPtr.Get() == false))
			{
				if (Result != EDownloadToMemoryResult::Success && Result != EDownloadToMemoryResult::SucceededByPayload)
				{
					UE_LOG(LogRuntimeFilesDownloader, Error, TEXT("Failed to download file chunk from %s: download failed. Trying to download the file by payload"), *URL);
					DownloadByPayload();
					OnChunkDownloadedFilled();
					return;
				}
				OverallDownloadedDataPtr->Shrink();
				PromisePtr->SetValue(FRuntimeChunkDownloaderResult{Result, MoveTemp(*OverallDownloadedDataPtr.Get())});
			}
		});
	});
	return PromisePtr->GetFuture();
}

TFuture<EDownloadToMemoryResult> FRuntimeChunkDownloader::DownloadFilePerChunk(const FString& URL, float Timeout, const FString& ContentType, int64 MaxChunkSize, FInt64Vector2 ChunkRange, const TFunction<void(int64, int64)>& OnProgress, const TFunction<void(TArray64<uint8>&&)>& OnChunkDownloaded, const TMap<FString, FString>& Headers)
{
	if (bCanceled)
	{
		UE_LOG(LogRuntimeFilesDownloader, Warning, TEXT("Canceled file chunk download from %s"), *URL);
		return MakeFulfilledPromise<EDownloadToMemoryResult>(EDownloadToMemoryResult::Cancelled).GetFuture();
	}

	TSharedPtr<TPromise<EDownloadToMemoryResult>> PromisePtr = MakeShared<TPromise<EDownloadToMemoryResult>>();
	TWeakPtr<FRuntimeChunkDownloader> WeakThisPtr = AsShared();
	GetContentSize(URL, Timeout, Headers).Next([WeakThisPtr, PromisePtr, URL, Timeout, ContentType, MaxChunkSize, OnProgress, OnChunkDownloaded, ChunkRange, Headers](int64 ContentSize) mutable
	{
		TSharedPtr<FRuntimeChunkDownloader> SharedThis = WeakThisPtr.Pin();
		if (!SharedThis.IsValid())
		{
			UE_LOG(LogRuntimeFilesDownloader, Warning, TEXT("Failed to download file chunk from %s: downloader has been destroyed"), *URL);
			PromisePtr->SetValue(EDownloadToMemoryResult::DownloadFailed);
			return;
		}
		
		if (SharedThis->bCanceled)
		{
			UE_LOG(LogRuntimeFilesDownloader, Warning, TEXT("Canceled file chunk download from %s"), *URL);
			PromisePtr->SetValue(EDownloadToMemoryResult::Cancelled);
			return;
		}

		if (ContentSize == -304)
		{
			PromisePtr->SetValue(EDownloadToMemoryResult::NotModified);
			return;
		}

		if (ContentSize <= 0)
		{
			UE_LOG(LogRuntimeFilesDownloader, Warning, TEXT("Unable to get content size for %s. Trying to download the file by payload"), *URL);
			SharedThis->DownloadFileByPayload(URL, Timeout, ContentType, OnProgress, Headers).Next([WeakThisPtr, PromisePtr, URL, Timeout, ContentType, OnChunkDownloaded, OnProgress](FRuntimeChunkDownloaderResult Result) mutable
			{
				TSharedPtr<FRuntimeChunkDownloader> SharedThis = WeakThisPtr.Pin();
				if (!SharedThis.IsValid())
				{
					UE_LOG(LogRuntimeFilesDownloader, Warning, TEXT("Failed to download file chunk from %s: downloader has been destroyed"), *URL);
					PromisePtr->SetValue(EDownloadToMemoryResult::DownloadFailed);
					return;
				}

				if (SharedThis->bCanceled)
				{
					UE_LOG(LogRuntimeFilesDownloader, Warning, TEXT("Canceled file chunk download from %s"), *URL);
					PromisePtr->SetValue(EDownloadToMemoryResult::Cancelled);
					return;
				}

				if (Result.Result != EDownloadToMemoryResult::Success && Result.Result != EDownloadToMemoryResult::SucceededByPayload)
				{
					UE_LOG(LogRuntimeFilesDownloader, Error, TEXT("Failed to download file chunk from %s: %s"), *URL, *UEnum::GetValueAsString(Result.Result));
					PromisePtr->SetValue(Result.Result);
					return;
				}

				if (Result.Data.Num() <= 0)
				{
					UE_LOG(LogRuntimeFilesDownloader, Error, TEXT("Failed to download file chunk from %s: downloaded content is empty"), *URL);
					PromisePtr->SetValue(EDownloadToMemoryResult::DownloadFailed);
					return;
				}

				PromisePtr->SetValue(Result.Result);
				OnChunkDownloaded(MoveTemp(Result.Data));
			});
			return;
		}

		if (MaxChunkSize <= 0)
		{
			UE_LOG(LogRuntimeFilesDownloader, Error, TEXT("Failed to download file chunk from %s: max chunk size is <= 0"), *URL);
			PromisePtr->SetValue(EDownloadToMemoryResult::DownloadFailed);
			return;
		}

		// If the chunk range is not specified, determine the range based on the max chunk size and the content size
		if (ChunkRange.X == 0 && ChunkRange.Y == 0)
		{
			ChunkRange.Y = FMath::Min(MaxChunkSize, ContentSize) - 1;
		}

		if (ChunkRange.Y > ContentSize)
		{
			UE_LOG(LogRuntimeFilesDownloader, Error, TEXT("Failed to download file chunk from %s: chunk range is out of range (%lld, expected [0, %lld])"), *URL, ChunkRange.Y, ContentSize);
			PromisePtr->SetValue(EDownloadToMemoryResult::DownloadFailed);
			return;
		}

		auto OnProgressInternal = [WeakThisPtr, PromisePtr, URL, Timeout, ContentType, MaxChunkSize, OnChunkDownloaded, OnProgress, ChunkRange](int64 BytesReceived, int64 ContentSize) mutable
		{
			TSharedPtr<FRuntimeChunkDownloader> SharedThis = WeakThisPtr.Pin();
			if (SharedThis.IsValid())
			{
				const float Progress = ContentSize <= 0 ? 0.0f : static_cast<float>(BytesReceived + ChunkRange.X) / ContentSize;
				UE_LOG(LogRuntimeFilesDownloader, Log, TEXT("Downloaded %lld bytes of file chunk from %s. Range: {%lld; %lld}, Overall: %lld, Progress: %f"), BytesReceived, *URL, ChunkRange.X, ChunkRange.Y, ContentSize, Progress);
				OnProgress(BytesReceived + ChunkRange.X, ContentSize);
			}
		};

		SharedThis->DownloadFileByChunk(URL, Timeout, ContentType, ContentSize, ChunkRange, OnProgressInternal).Next([WeakThisPtr, PromisePtr, URL, Timeout, ContentType, ContentSize, MaxChunkSize, OnChunkDownloaded, OnProgress, ChunkRange, Headers](FRuntimeChunkDownloaderResult&& Result)
		{
			TSharedPtr<FRuntimeChunkDownloader> SharedThis = WeakThisPtr.Pin();
			if (!SharedThis.IsValid())
			{
				UE_LOG(LogRuntimeFilesDownloader, Warning, TEXT("Failed to download file chunk from %s: downloader has been destroyed"), *URL);
				PromisePtr->SetValue(EDownloadToMemoryResult::DownloadFailed);
				return;
			}

			if (SharedThis->bCanceled)
			{
				UE_LOG(LogRuntimeFilesDownloader, Warning, TEXT("Canceled file chunk download from %s"), *URL);
				PromisePtr->SetValue(EDownloadToMemoryResult::Cancelled);
				return;
			}

			if (Result.Result != EDownloadToMemoryResult::Success && Result.Result != EDownloadToMemoryResult::SucceededByPayload)
			{
				UE_LOG(LogRuntimeFilesDownloader, Error, TEXT("Failed to download file chunk from %s: %s"), *URL, *UEnum::GetValueAsString(Result.Result));
				PromisePtr->SetValue(Result.Result);
				return;
			}

			OnChunkDownloaded(MoveTemp(Result.Data));

			// Check if the download is complete
			if (ContentSize > ChunkRange.Y + 1)
			{
				const int64 ChunkStart = ChunkRange.Y + 1;
				const int64 ChunkEnd = FMath::Min(ChunkStart + MaxChunkSize, ContentSize) - 1;

				SharedThis->DownloadFilePerChunk(URL, Timeout, ContentType, MaxChunkSize, FInt64Vector2(ChunkStart, ChunkEnd), OnProgress, OnChunkDownloaded, Headers).Next([WeakThisPtr, PromisePtr](EDownloadToMemoryResult Result)
				{
					PromisePtr->SetValue(Result);
				});
			}
			else
			{
				PromisePtr->SetValue(EDownloadToMemoryResult::Success);
			}
		});
	});

	return PromisePtr->GetFuture();
}

TFuture<FRuntimeChunkDownloaderResult> FRuntimeChunkDownloader::DownloadFileByChunk(const FString& URL, float Timeout, const FString& ContentType, int64 ContentSize, FInt64Vector2 ChunkRange, const TFunction<void(int64, int64)>& OnProgress)
{
	if (bCanceled)
	{
		UE_LOG(LogRuntimeFilesDownloader, Warning, TEXT("Canceled file download from %s"), *URL);
		return MakeFulfilledPromise<FRuntimeChunkDownloaderResult>(FRuntimeChunkDownloaderResult{EDownloadToMemoryResult::Cancelled, {}, {}}).GetFuture();
	}

	if (ChunkRange.X < 0 || ChunkRange.Y <= 0 || ChunkRange.X > ChunkRange.Y)
	{
		UE_LOG(LogRuntimeFilesDownloader, Error, TEXT("Failed to download file chunk from %s: chunk range (%lld; %lld) is invalid"), *URL, ChunkRange.X, ChunkRange.Y);
		return MakeFulfilledPromise<FRuntimeChunkDownloaderResult>(FRuntimeChunkDownloaderResult{EDownloadToMemoryResult::DownloadFailed, {}, {}}).GetFuture();
	}

	if (ChunkRange.Y - ChunkRange.X + 1 > ContentSize)
	{
		UE_LOG(LogRuntimeFilesDownloader, Error, TEXT("Failed to download file chunk from %s: chunk range (%lld; %lld) is out of range (%lld)"), *URL, ChunkRange.X, ChunkRange.Y, ContentSize);
		return MakeFulfilledPromise<FRuntimeChunkDownloaderResult>(FRuntimeChunkDownloaderResult{EDownloadToMemoryResult::DownloadFailed, {}, {}}).GetFuture();
	}

	TWeakPtr<FRuntimeChunkDownloader> WeakThisPtr = AsShared();

#if UE_VERSION_NEWER_THAN(4, 26, 0)
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequestRef = FHttpModule::Get().CreateRequest();
#else
	const TSharedRef<IHttpRequest> HttpRequestRef = FHttpModule::Get().CreateRequest();
#endif

	HttpRequestRef->SetVerb("GET");
	HttpRequestRef->SetURL(URL);

#if UE_VERSION_NEWER_THAN(4, 26, 0)
	HttpRequestRef->SetTimeout(Timeout);
#else
	UE_LOG(LogRuntimeFilesDownloader, Warning, TEXT("The Timeout feature is only supported in engine version 4.26 or later. Please update your engine to use this feature"));
#endif

	if (!ContentType.IsEmpty())
	{
		HttpRequestRef->SetHeader(TEXT("Content-Type"), ContentType);
	}

	const FString RangeHeaderValue = FString::Format(TEXT("bytes={0}-{1}"), {ChunkRange.X, ChunkRange.Y});
	HttpRequestRef->SetHeader(TEXT("Range"), RangeHeaderValue);

	HttpRequestRef->OnRequestProgress().BindLambda([WeakThisPtr, ContentSize, ChunkRange, OnProgress](FHttpRequestPtr Request, int32 BytesSent, int32 BytesReceived)
	{
		TSharedPtr<FRuntimeChunkDownloader> SharedThis = WeakThisPtr.Pin();
		if (SharedThis.IsValid())
		{
			const float Progress = ContentSize <= 0 ? 0.0f : static_cast<float>(BytesReceived) / ContentSize;
			UE_LOG(LogRuntimeFilesDownloader, Log, TEXT("Downloaded %d bytes of file chunk from %s. Range: {%lld; %lld}, Overall: %lld, Progress: %f"), BytesReceived, *Request->GetURL(), ChunkRange.X, ChunkRange.Y, ContentSize, Progress);
			OnProgress(BytesReceived, ContentSize);
		}
	});

	TSharedPtr<TPromise<FRuntimeChunkDownloaderResult>> PromisePtr = MakeShared<TPromise<FRuntimeChunkDownloaderResult>>();
	HttpRequestRef->OnProcessRequestComplete().BindLambda([WeakThisPtr, PromisePtr, URL, ChunkRange](FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSuccess) mutable
	{
		TSharedPtr<FRuntimeChunkDownloader> SharedThis = WeakThisPtr.Pin();
		if (!SharedThis.IsValid())
		{
			UE_LOG(LogRuntimeFilesDownloader, Warning, TEXT("Failed to download file chunk from %s: downloader has been destroyed"), *URL);
			PromisePtr->SetValue(FRuntimeChunkDownloaderResult{EDownloadToMemoryResult::DownloadFailed, {}, Response->GetAllHeaders()});
			return;
		}

		if (SharedThis->bCanceled)
		{
			UE_LOG(LogRuntimeFilesDownloader, Warning, TEXT("Canceled file chunk download from %s"), *URL);
			PromisePtr->SetValue(FRuntimeChunkDownloaderResult{EDownloadToMemoryResult::Cancelled, {}, Response->GetAllHeaders()});
			return;
		}

		if (!bSuccess || !Response.IsValid())
		{
			UE_LOG(LogRuntimeFilesDownloader, Error, TEXT("Failed to download file chunk from %s: request failed"), *Request->GetURL());
			PromisePtr->SetValue(FRuntimeChunkDownloaderResult{EDownloadToMemoryResult::DownloadFailed, {}, Response->GetAllHeaders()});
			return;
		}

		if (Response->GetResponseCode() / 100 != 2)
		{

			if (Response->GetResponseCode() == 304)
			{
				UE_LOG(LogRuntimeFilesDownloader, Log, TEXT("Response code to GET for downloading file chunk from %s by payload: %d %s"), *URL, Response->GetResponseCode(), *Response->GetContentAsString());
				PromisePtr->SetValue(FRuntimeChunkDownloaderResult{EDownloadToMemoryResult::NotModified, {}, Response->GetAllHeaders()});
			}
			else
			{
				UE_LOG(LogRuntimeFilesDownloader, Error, TEXT("Response code to GET for downloading file chunk from %s by payload: %d %s"), *URL, Response->GetResponseCode(), *Response->GetContentAsString());
				PromisePtr->SetValue(FRuntimeChunkDownloaderResult{EDownloadToMemoryResult::DownloadFailed, {}, Response->GetAllHeaders()});
			}
			return;
		}

		if (Response->GetContentLength() <= 0)
		{
			UE_LOG(LogRuntimeFilesDownloader, Error, TEXT("Failed to download file chunk from %s: content length is 0"), *Request->GetURL());
			PromisePtr->SetValue(FRuntimeChunkDownloaderResult{EDownloadToMemoryResult::DownloadFailed, {}, Response->GetAllHeaders()});
			return;
		}

		const int64 ContentLength = FCString::Atoi64(*Response->GetHeader("Content-Length"));

		if (ContentLength != ChunkRange.Y - ChunkRange.X + 1)
		{
			UE_LOG(LogRuntimeFilesDownloader, Error, TEXT("Failed to download file chunk from %s: content length (%lld) does not match the expected length (%lld)"), *Request->GetURL(), ContentLength, ChunkRange.Y - ChunkRange.X + 1);
			PromisePtr->SetValue(FRuntimeChunkDownloaderResult{EDownloadToMemoryResult::DownloadFailed, {}, Response->GetAllHeaders()});
			return;
		}

		UE_LOG(LogRuntimeFilesDownloader, Log, TEXT("Successfully downloaded file chunk from %s. Range: {%lld; %lld}, Overall: %lld"), *Request->GetURL(), ChunkRange.X, ChunkRange.Y, ContentLength);
		PromisePtr->SetValue(FRuntimeChunkDownloaderResult{EDownloadToMemoryResult::Success, TArray64<uint8>(Response->GetContent()), Response->GetAllHeaders()});
	});

	if (!HttpRequestRef->ProcessRequest())
	{
		UE_LOG(LogRuntimeFilesDownloader, Error, TEXT("Failed to download file chunk from %s: request failed"), *URL);
		return MakeFulfilledPromise<FRuntimeChunkDownloaderResult>(FRuntimeChunkDownloaderResult{EDownloadToMemoryResult::DownloadFailed, {}, {}}).GetFuture();
	}

	HttpRequestPtr = HttpRequestRef;
	return PromisePtr->GetFuture();
}

TFuture<FRuntimeChunkDownloaderResult> FRuntimeChunkDownloader::DownloadFileByPayload(const FString& URL, float Timeout, const FString& ContentType, const TFunction<void(int64, int64)>& OnProgress, const TMap<FString, FString>& Headers)
{
	if (bCanceled)
	{
		UE_LOG(LogRuntimeFilesDownloader, Warning, TEXT("Canceled file download from %s"), *URL);
		return MakeFulfilledPromise<FRuntimeChunkDownloaderResult>(FRuntimeChunkDownloaderResult{EDownloadToMemoryResult::Cancelled, {}, {}}).GetFuture();
	}

	TWeakPtr<FRuntimeChunkDownloader> WeakThisPtr = AsShared();

#if UE_VERSION_NEWER_THAN(4, 26, 0)
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequestRef = FHttpModule::Get().CreateRequest();
#else
	const TSharedRef<IHttpRequest> HttpRequestRef = FHttpModule::Get().CreateRequest();
#endif

	HttpRequestRef->SetVerb("GET");
	HttpRequestRef->SetURL(URL);
	for (const auto& [Key, Value] : Headers)
	{
		HttpRequestRef->SetHeader(Key, Value);
	}

#if UE_VERSION_NEWER_THAN(4, 26, 0)
	HttpRequestRef->SetTimeout(Timeout);
#else
	UE_LOG(LogRuntimeFilesDownloader, Warning, TEXT("The Timeout feature is only supported in engine version 4.26 or later. Please update your engine to use this feature"));
#endif

	HttpRequestRef->OnRequestProgress().BindLambda([WeakThisPtr, OnProgress](FHttpRequestPtr Request, int32 BytesSent, int32 BytesReceived)
	{
		TSharedPtr<FRuntimeChunkDownloader> SharedThis = WeakThisPtr.Pin();
		if (SharedThis.IsValid())
		{
			const int64 ContentLength = Request->GetContentLength();
			const float Progress = ContentLength <= 0 ? 0.0f : static_cast<float>(BytesReceived) / ContentLength;
			UE_LOG(LogRuntimeFilesDownloader, Log, TEXT("Downloaded %d bytes of file chunk from %s by payload. Overall: %lld, Progress: %f"), BytesReceived, *Request->GetURL(), static_cast<int64>(Request->GetContentLength()), Progress);
			OnProgress(BytesReceived, ContentLength);
		}
	});

	TSharedPtr<TPromise<FRuntimeChunkDownloaderResult>> PromisePtr = MakeShared<TPromise<FRuntimeChunkDownloaderResult>>();
	HttpRequestRef->OnProcessRequestComplete().BindLambda([WeakThisPtr, PromisePtr, URL](FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSuccess) mutable
	{
		TSharedPtr<FRuntimeChunkDownloader> SharedThis = WeakThisPtr.Pin();
		TArray<FString> ResponseHeaders = Response ? Response->GetAllHeaders() : TArray<FString>();
		if (!SharedThis.IsValid())
		{
			UE_LOG(LogRuntimeFilesDownloader, Warning, TEXT("Failed to download file from %s by payload: downloader has been destroyed"), *URL);
			PromisePtr->SetValue(FRuntimeChunkDownloaderResult{EDownloadToMemoryResult::DownloadFailed, {}, ResponseHeaders});
			return;
		}

		if (SharedThis->bCanceled)
		{
			UE_LOG(LogRuntimeFilesDownloader, Warning, TEXT("Canceled file download from %s by payload"), *URL);
			PromisePtr->SetValue(FRuntimeChunkDownloaderResult{EDownloadToMemoryResult::Cancelled, {}, ResponseHeaders});
			return;
		}

		if (!bSuccess || !Response.IsValid())
		{
			UE_LOG(LogRuntimeFilesDownloader, Error, TEXT("Failed to download file from %s by payload: request failed"), *Request->GetURL());
			PromisePtr->SetValue(FRuntimeChunkDownloaderResult{EDownloadToMemoryResult::DownloadFailed, {}, ResponseHeaders });
			return;
		}
		if (Response->GetResponseCode() / 100 != 2)
		{
			if (Response->GetResponseCode() == 304)
			{
				UE_LOG(LogRuntimeFilesDownloader, Log, TEXT("Response code to GET for downloading file from %s by payload: %d %s"), *URL, Response->GetResponseCode(), *Response->GetContentAsString());
				PromisePtr->SetValue(FRuntimeChunkDownloaderResult{EDownloadToMemoryResult::NotModified, {}, ResponseHeaders});
			}
			else
			{
				UE_LOG(LogRuntimeFilesDownloader, Error, TEXT("Response code to GET for downloading file from %s by payload: %d %s"), *URL, Response->GetResponseCode(), *Response->GetContentAsString());
				PromisePtr->SetValue(FRuntimeChunkDownloaderResult{EDownloadToMemoryResult::DownloadFailed, {}, ResponseHeaders});
			}
			return;
		}

		if (Response->GetContentLength() <= 0)
		{
			UE_LOG(LogRuntimeFilesDownloader, Error, TEXT("Failed to download file from %s by payload: content length is 0"), *Request->GetURL());
			PromisePtr->SetValue(FRuntimeChunkDownloaderResult{EDownloadToMemoryResult::DownloadFailed, {}, ResponseHeaders});
			return;
		}

		UE_LOG(LogRuntimeFilesDownloader, Log, TEXT("Successfully downloaded file from %s by payload. Overall: %lld"), *Request->GetURL(), static_cast<int64>(Response->GetContentLength()));
		return PromisePtr->SetValue(FRuntimeChunkDownloaderResult{EDownloadToMemoryResult::SucceededByPayload, TArray64<uint8>(Response->GetContent()), ResponseHeaders});
	});

	if (!HttpRequestRef->ProcessRequest())
	{
		UE_LOG(LogRuntimeFilesDownloader, Error, TEXT("Failed to download file from %s by payload: request failed"), *URL);
		return MakeFulfilledPromise<FRuntimeChunkDownloaderResult>(FRuntimeChunkDownloaderResult{EDownloadToMemoryResult::DownloadFailed, {}, {}}).GetFuture();
	}

	HttpRequestPtr = HttpRequestRef;
	return PromisePtr->GetFuture();
}

TFuture<int64> FRuntimeChunkDownloader::GetContentSize(const FString& URL, float Timeout, const TMap<FString, FString>& Headers)
{
	TSharedPtr<TPromise<int64>> PromisePtr = MakeShared<TPromise<int64>>();

#if UE_VERSION_NEWER_THAN(4, 26, 0)
	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequestRef = FHttpModule::Get().CreateRequest();
#else
	const TSharedRef<IHttpRequest> HttpRequestRef = FHttpModule::Get().CreateRequest();
#endif

	HttpRequestRef->SetVerb("HEAD");
	HttpRequestRef->SetURL(URL);
	for (const auto& [Key, Value] : Headers)
	{
		HttpRequestRef->SetHeader(Key, Value);
	}

#if UE_VERSION_NEWER_THAN(4, 26, 0)
	HttpRequestRef->SetTimeout(Timeout);
#else
	UE_LOG(LogRuntimeFilesDownloader, Warning, TEXT("The Timeout feature is only supported in engine version 4.26 or later. Please update your engine to use this feature"));
#endif

	HttpRequestRef->OnProcessRequestComplete().BindLambda([PromisePtr, URL](const FHttpRequestPtr& Request, const FHttpResponsePtr& Response, const bool bSucceeded)
	{
		if (!bSucceeded || !Response.IsValid())
		{
			UE_LOG(LogRuntimeFilesDownloader, Error, TEXT("Failed to get size of file from %s: request failed"), *URL);
			PromisePtr->SetValue(0);
			return;
		}
		if (Response->GetResponseCode() / 100 != 2)
		{
			if (Response->GetResponseCode() == 304)
			{
				UE_LOG(LogRuntimeFilesDownloader, Log, TEXT("Response code to GET for downloading file chunk from %s by payload: %d %s"), *URL, Response->GetResponseCode(), *Response->GetContentAsString());
				PromisePtr->SetValue(-304);
			}
			else
			{
				UE_LOG(LogRuntimeFilesDownloader, Error, TEXT("Response code to GET for downloading file chunk from %s by payload: %d %s"), *URL, Response->GetResponseCode(), *Response->GetContentAsString());
				PromisePtr->SetValue(0);
			}
			return;
		}

		const int64 ContentLength = FCString::Atoi64(*Response->GetHeader("Content-Length"));
		if (ContentLength <= 0)
		{
			UE_LOG(LogRuntimeFilesDownloader, Error, TEXT("Failed to get size of file from %s: content length is %lld, expected > 0"), *URL, ContentLength);
			PromisePtr->SetValue(0);
			return;
		}

		UE_LOG(LogRuntimeFilesDownloader, Log, TEXT("Got size of file from %s: %lld"), *URL, ContentLength);
		PromisePtr->SetValue(ContentLength);
	});

	if (!HttpRequestRef->ProcessRequest())
	{
		UE_LOG(LogRuntimeFilesDownloader, Error, TEXT("Failed to get size of file from %s: request failed"), *URL);
		return MakeFulfilledPromise<int64>(0).GetFuture();
	}

	HttpRequestPtr = HttpRequestRef;
	return PromisePtr->GetFuture();
}

void FRuntimeChunkDownloader::CancelDownload()
{
	bCanceled = true;
	if (HttpRequestPtr.IsValid())
	{
#if UE_VERSION_NEWER_THAN(4, 26, 0)
		const TSharedPtr<IHttpRequest, ESPMode::ThreadSafe> HttpRequest = HttpRequestPtr.Pin();
#else
		const TSharedPtr<IHttpRequest> HttpRequest = HttpRequestPtr.Pin();
#endif

		HttpRequest->CancelRequest();
	}
	UE_LOG(LogRuntimeFilesDownloader, Warning, TEXT("Download canceled"));
}

TFuture<FRuntimeChunkUploaderResult> FRuntimeChunkDownloader::UploadFile(
	const FString& URL, float Timeout, TArray<uint8>& Body, const TFunction<void(int64, int64)>& OnProgress, const TMap<FString, FString>& Headers)
{
	TWeakPtr<FRuntimeChunkDownloader> WeakThisPtr = AsShared();

	const TSharedRef<IHttpRequest, ESPMode::ThreadSafe> HttpRequestRef = FHttpModule::Get().CreateRequest();
	HttpRequestRef->SetVerb("PUT");
	HttpRequestRef->SetURL(URL);
	HttpRequestRef->SetTimeout(Timeout);
	for (const auto& [Key, Value] : Headers)
	{
		HttpRequestRef->SetHeader(Key, Value);
	}
	HttpRequestRef->SetContent(Body);
	auto ContentSize = Body.Num();

	HttpRequestRef->OnRequestProgress().BindLambda(
		[WeakThisPtr, ContentSize, OnProgress](FHttpRequestPtr Request, int32 BytesSent, int32 BytesReceived) {
			TSharedPtr<FRuntimeChunkDownloader> SharedThis = WeakThisPtr.Pin();
			if (SharedThis.IsValid())
			{
				const float Progress = ContentSize <= 0 ? 0.0f : static_cast<float>(BytesSent) / ContentSize;
				UE_LOG(LogRuntimeFilesDownloader, Log,
					TEXT("Uploaded %d bytes of file to %s. Overall: %lld, Progress: %.2f"), BytesSent,
					*Request->GetURL(), ContentSize, Progress);
				OnProgress(BytesSent, ContentSize);
			}
		});

	TSharedPtr<TPromise<FRuntimeChunkUploaderResult>> PromisePtr = MakeShared<TPromise<
		FRuntimeChunkUploaderResult>>();
	HttpRequestRef->OnProcessRequestComplete().BindLambda(
		[WeakThisPtr, PromisePtr, URL](FHttpRequestPtr Request, FHttpResponsePtr Response, bool bSuccess) mutable {
			TSharedPtr<FRuntimeChunkDownloader> SharedThis = WeakThisPtr.Pin();
			if (!SharedThis.IsValid())
			{
				UE_LOG(LogRuntimeFilesDownloader, Warning,
					TEXT("Failed to upload file to %s: uploader has been destroyed"), *URL);
				PromisePtr->SetValue(FRuntimeChunkUploaderResult{ EUploadFromStorageResult::UploadFailed });
				return;
			}
			if (SharedThis->bCanceled)
			{
				UE_LOG(LogRuntimeFilesDownloader, Warning, TEXT("Canceled file upload to %s"), *URL);
				PromisePtr->SetValue(FRuntimeChunkUploaderResult{ EUploadFromStorageResult::Cancelled });
				return;
			}

			if (!bSuccess || !Response.IsValid())
			{
				UE_LOG(LogRuntimeFilesDownloader, Error, TEXT("Failed to upload file to %s: request failed"),
					*Request->GetURL());
				PromisePtr->SetValue(FRuntimeChunkUploaderResult{ EUploadFromStorageResult::UploadFailed });
				return;
			}

			if (Response->GetResponseCode() != 200)
			{
				auto ResponseText = Response->GetContentAsString();
				UE_LOG(LogRuntimeFilesDownloader, Error, TEXT("Failed to upload file to %s: %d %s"), *Request->GetURL(),
					Response->GetResponseCode(), *ResponseText);
				PromisePtr->SetValue(FRuntimeChunkUploaderResult{ EUploadFromStorageResult::UploadFailed });
				return;
			}

			auto ResponseText = Response->GetContentAsString();
			UE_LOG(LogRuntimeFilesDownloader, Display, TEXT("Successfully uploaded file to %s: %d %s"),
				*Request->GetURL(), Response->GetResponseCode(), *ResponseText);

			PromisePtr->SetValue(FRuntimeChunkUploaderResult{ EUploadFromStorageResult::Success });
		});

	if (!HttpRequestRef->ProcessRequest())
	{
		UE_LOG(LogRuntimeFilesDownloader, Error, TEXT("Failed to upload file to %s: request failed"), *URL);
		return MakeFulfilledPromise<FRuntimeChunkUploaderResult>(FRuntimeChunkUploaderResult{
			EUploadFromStorageResult::UploadFailed
		}).GetFuture();
	}

	HttpRequestPtr = HttpRequestRef;
	return PromisePtr->GetFuture();
}
