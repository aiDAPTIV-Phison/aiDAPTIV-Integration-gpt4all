#include "localdocs.h"

#include "chatapi.h"
#include "chatlistmodel.h"
#include "chatllm.h"  // 包含 LLModelInfo 的定義
#include "chatmodel.h"
#include "database.h"
#include "embllm.h"
#include "mysettings.h"
#include "modellist.h"
#include "network.h"

#include <QCoreApplication>
#include <QDebug>
#include <QGlobalStatic>
#include <QGuiApplication>
#include <QList>
#include <QThread>
#include <QUrl>
#include <Qt>
#include <QtLogging>
#include <gpt4all-backend/llmodel.h>

// LLModelStore 定義在 chatllm.cpp 中，但 globalInstance() 是公開的
// 我們需要前向聲明 LLModelStore 類
class LLModelStore {
public:
    static LLModelStore *globalInstance();
    LLModelInfo acquireModel();
    void releaseModel(LLModelInfo &&info);
};


class MyLocalDocs: public LocalDocs { };
Q_GLOBAL_STATIC(MyLocalDocs, localDocsInstance)
LocalDocs *LocalDocs::globalInstance()
{
    return localDocsInstance();
}

LocalDocs::LocalDocs()
    : QObject(nullptr)
    , m_localDocsModel(new LocalDocsModel(this))
    , m_database(nullptr)
{
    connect(MySettings::globalInstance(), &MySettings::localDocsChunkSizeChanged, this, &LocalDocs::handleChunkSizeChanged);
    connect(MySettings::globalInstance(), &MySettings::localDocsFileExtensionsChanged, this, &LocalDocs::handleFileExtensionsChanged);

    // Create the DB with the chunk size from settings
    m_database = new Database(MySettings::globalInstance()->localDocsChunkSize(),
                              MySettings::globalInstance()->localDocsFileExtensions());

    connect(this, &LocalDocs::requestStart, m_database,
        &Database::start, Qt::QueuedConnection);
    connect(this, &LocalDocs::requestForceIndexing, m_database,
        &Database::forceIndexing, Qt::QueuedConnection);
    connect(this, &LocalDocs::forceRebuildFolder, m_database,
        &Database::forceRebuildFolder, Qt::QueuedConnection);
    connect(this, &LocalDocs::requestAddFolder, m_database,
        &Database::addFolder, Qt::QueuedConnection);
    connect(this, &LocalDocs::requestRemoveFolder, m_database,
        &Database::removeFolder, Qt::QueuedConnection);
    connect(this, &LocalDocs::requestChunkSizeChange, m_database,
        &Database::changeChunkSize, Qt::QueuedConnection);
    connect(this, &LocalDocs::requestFileExtensionsChange, m_database,
        &Database::changeFileExtensions, Qt::QueuedConnection);
    connect(m_database, &Database::databaseValidChanged,
        this, &LocalDocs::databaseValidChanged, Qt::QueuedConnection);

    // Connections for modifying the model and keeping it updated with the database
    connect(m_database, &Database::requestUpdateGuiForCollectionItem,
        m_localDocsModel, &LocalDocsModel::updateCollectionItem, Qt::QueuedConnection);
    connect(m_database, &Database::requestAddGuiCollectionItem,
        m_localDocsModel, &LocalDocsModel::addCollectionItem, Qt::QueuedConnection);
    connect(m_database, &Database::requestRemoveGuiFolderById,
        m_localDocsModel, &LocalDocsModel::removeFolderById, Qt::QueuedConnection);
    connect(m_database, &Database::requestGuiCollectionListUpdated,
        m_localDocsModel, &LocalDocsModel::collectionListUpdated, Qt::QueuedConnection);
    connect(m_database, &Database::requestProcessDocumentWithLLM,
        this, &LocalDocs::handleProcessDocumentWithLLM, Qt::QueuedConnection);

    connect(qGuiApp, &QCoreApplication::aboutToQuit, this, &LocalDocs::aboutToQuit);
}

void LocalDocs::aboutToQuit()
{
    delete m_database;
    m_database = nullptr;
}

void LocalDocs::addFolder(const QString &collection, const QString &path)
{
    const QUrl url(path);
    const QString localPath = url.isLocalFile() ? url.toLocalFile() : path;

    const QString embedding_model = EmbeddingLLM::model();
    if (embedding_model.isEmpty()) {
        qWarning() << "ERROR: We have no embedding model";
        return;
    }

    emit requestAddFolder(collection, localPath, embedding_model);
}

void LocalDocs::removeFolder(const QString &collection, const QString &path)
{
    emit requestRemoveFolder(collection, path);
}

void LocalDocs::forceIndexing(const QString &collection)
{
    const QString embedding_model = EmbeddingLLM::model();
    if (embedding_model.isEmpty()) {
        qWarning() << "ERROR: We have no embedding model";
        return;
    }

    emit requestForceIndexing(collection, embedding_model);
}

void LocalDocs::handleChunkSizeChanged()
{
    emit requestChunkSizeChange(MySettings::globalInstance()->localDocsChunkSize());
}

void LocalDocs::handleFileExtensionsChanged()
{
    emit requestFileExtensionsChange(MySettings::globalInstance()->localDocsFileExtensions());
}

void LocalDocs::handleProcessDocumentWithLLM(const QString &collection, const QString &fullContent)
{
    qDebug() << "LocalDocs: Processing document with LLM, collection:" << collection;
    qDebug() << "Document content length:" << fullContent.length();
    
    // 增加 LLM 調用計數（防止 UI 過早顯示 Ready）
    m_database->incrementLLMCallCount(collection);
    
    // 嘗試從 LLModelStore 獲取當前加載的模型
    try {
        LLModelStore *store = LLModelStore::globalInstance();
        if (!store) {
            qWarning() << "LocalDocs: LLModelStore not available";
            m_database->decrementLLMCallCount(collection);
            return;
        }
        
        // 獲取當前模型設置（需要在獲取模型之前獲取，因為可能需要觸發模型加載）
        ModelList *modelList = ModelList::globalInstance();
        if (!modelList) {
            qWarning() << "LocalDocs: ModelList not available";
            m_database->decrementLLMCallCount(collection);
            return;
        }
        
        // 獲取默認模型信息
        ModelInfo defaultModel = modelList->defaultModelInfo();
        if (defaultModel.filename() == "") {
            qWarning() << "LocalDocs: No default model available";
            m_database->decrementLLMCallCount(collection);
            return;
        }
        
        // 記錄使用的模型信息
        qDebug() << "LocalDocs: Using model - Name:" << defaultModel.name() 
                 << ", Filename:" << defaultModel.filename()
                 << ", IsOnline:" << defaultModel.isOnline;
        
        // 檢查是否為 API 模型
        bool isAPIModel = defaultModel.isOnline;
        LLModelInfo modelInfo;
        bool needReleaseToStore = false;
        
        if (isAPIModel) {
            // API 模型：直接創建 ChatAPI 對象，不需要通過 store（因為只是打 HTTP endpoint）
            qDebug() << "LocalDocs: Using API model, creating ChatAPI directly (no store needed)";
            QString filePath = defaultModel.dirpath + defaultModel.filename();
            QFile file(filePath);
            if (!file.open(QIODeviceBase::ReadOnly)) {
                qWarning() << "LocalDocs: Cannot open API model file:" << filePath;
                m_database->decrementLLMCallCount(collection);
                return;
            }
            
            QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
            QJsonObject obj = doc.object();
            QString apiKey = obj["apiKey"].toString();
            QString modelName = obj["modelName"].toString();
            QString requestUrl;
            
            if (defaultModel.isCompatibleApi) {
                QString baseUrl(obj["baseUrl"].toString());
                QUrl apiUrl(QUrl::fromUserInput(baseUrl));
                if (!Network::isHttpUrlValid(apiUrl)) {
                    qWarning() << "LocalDocs: Invalid API URL:" << baseUrl;
                    m_database->decrementLLMCallCount(collection);
                    return;
                }
                QString currentPath(apiUrl.path());
                QString suffixPath("%1/chat/completions");
                apiUrl.setPath(suffixPath.arg(currentPath));
                requestUrl = apiUrl.toString();
            } else {
                requestUrl = defaultModel.url();
            }
            
            ChatAPI *apiModel = new ChatAPI();
            apiModel->setModelName(modelName);
            apiModel->setRequestURL(requestUrl);
            apiModel->setAPIKey(apiKey);
            
            // 記錄 API 模型的 endpoint 信息
            qDebug() << "LocalDocs: API Model Endpoint - URL:" << requestUrl 
                     << ", ModelName:" << modelName;
            
            // 創建 LLModelInfo（不需要通過 store）
            modelInfo.model.reset(apiModel);
            modelInfo.fileInfo = QFileInfo(filePath);
            needReleaseToStore = false;  // API 模型不需要釋放回 store
            
            qDebug() << "LocalDocs: ChatAPI created successfully, ready to use";
        } else {
            // 本地模型：需要通過 store 獲取（因為需要加載模型文件）
            qDebug() << "LocalDocs: Using local model, acquiring from store";
            qDebug() << "LocalDocs: Local Model - FilePath:" << (defaultModel.dirpath + defaultModel.filename());
            modelInfo = store->acquireModel();
            if (!modelInfo.model || !modelInfo.model->isModelLoaded()) {
                qWarning() << "LocalDocs: No model available in store, attempting to load default model";
                store->releaseModel(std::move(modelInfo));
                
                // 嘗試觸發默認模型加載
                ChatListModel *chatListModel = ChatListModel::globalInstance();
                if (chatListModel && chatListModel->currentChat()) {
                    Chat *currentChat = chatListModel->currentChat();
                    if (currentChat && !currentChat->isModelLoaded()) {
                        qDebug() << "LocalDocs: Triggering default model load:" << defaultModel.filename();
                        currentChat->modelChangeRequested(defaultModel);
                        // 等待模型加載（簡單的輪詢，最多等待 30 秒）
                        for (int i = 0; i < 60 && !currentChat->isModelLoaded(); ++i) {
                            QThread::msleep(500);
                            QCoreApplication::processEvents();
                        }
                        if (currentChat->isModelLoaded()) {
                            qDebug() << "LocalDocs: Model loaded successfully";
                            // 檢查 chat 是否正在使用模型
                            if (currentChat->responseInProgress()) {
                                qWarning() << "LocalDocs: Chat is currently using the model, skipping LLM call";
                                m_database->decrementLLMCallCount(collection);
                                return;
                            }
                            // 暫時卸載模型，讓它被釋放到 store
                            currentChat->unloadModel();
                            // 等待模型被釋放
                            for (int i = 0; i < 10; ++i) {
                                QThread::msleep(500);
                                QCoreApplication::processEvents();
                            }
                            modelInfo = store->acquireModel();
                            if (!modelInfo.model || !modelInfo.model->isModelLoaded()) {
                                qWarning() << "LocalDocs: Model acquired but not valid";
                                store->releaseModel(std::move(modelInfo));
                                m_database->decrementLLMCallCount(collection);
                                return;
                            }
                            qDebug() << "LocalDocs: Local model acquired successfully from store";
                            needReleaseToStore = true;  // 本地模型需要釋放回 store
                        } else {
                            qWarning() << "LocalDocs: Model loading timeout";
                            m_database->decrementLLMCallCount(collection);
                            return;
                        }
                    } else {
                        qWarning() << "LocalDocs: Current chat already has model or no current chat";
                        m_database->decrementLLMCallCount(collection);
                        return;
                    }
                } else {
                    qWarning() << "LocalDocs: ChatListModel not available or no current chat";
                    m_database->decrementLLMCallCount(collection);
                    return;
                }
            } else {
                qDebug() << "LocalDocs: Local model already available in store";
                needReleaseToStore = true;  // 本地模型需要釋放回 store
            }
        }
        
        // 記錄最終使用的模型類型
        if (isAPIModel) {
            qDebug() << "LocalDocs: Final model type - API Model";
        } else {
            qDebug() << "LocalDocs: Final model type - Local Model";
        }
        
        ModelInfo currentModel = modelList->defaultModelInfo();
        auto *mySettings = MySettings::globalInstance();
        
        // 創建 prompt context（max_tokens 設為 1）
        LLModel::PromptContext ctx = {
            .n_predict      = 1,  // max_tokens 設為 1
            .top_k          = mySettings->modelTopK(currentModel),
            .top_p          = float(mySettings->modelTopP(currentModel)),
            .min_p          = float(mySettings->modelMinP(currentModel)),
            .temp           = float(mySettings->modelTemperature(currentModel)),
            .n_batch        = mySettings->modelPromptBatchSize(currentModel),
            .repeat_penalty = float(mySettings->modelRepeatPenalty(currentModel)),
            .repeat_last_n  = mySettings->modelRepeatPenaltyTokens(currentModel),
        };
        
        // 構建 prompt：格式與 chat LocalDocs 回傳時的 system prompt 一致
        // 格式：### Context from LocalDocs:\nCollection: ...\nFull Document Content:\n...
        // 不包含用戶設定的 system prompt，也不包含 user prompt
        QStringList localDocsParts;
        localDocsParts << u"### Context from LocalDocs:\n"_s
                      << u"Collection: "_s << collection
                      << u"\nFull Document Content:\n"_s << fullContent << u"\n\n"_s;
        QString promptContent = localDocsParts.join(QString());
        
        // 檢查是否為 API 模型，如果是則需要 XML 格式
        // 重用之前定義的 isAPIModel 變數（或者通過 dynamic_cast 確認）
        bool isAPIModelForPrompt = dynamic_cast<ChatAPI*>(modelInfo.model.get()) != nullptr;
        std::string promptStr;
        
        if (isAPIModelForPrompt) {
            // API 模型需要 XML 格式：<chat><system>...</system></chat>
            QString xmlPrompt = u"<chat><system>"_s + promptContent.toHtmlEscaped() + u"</system></chat>"_s;
            promptStr = xmlPrompt.toStdString();
        } else {
            // 本地模型直接使用文本格式
            promptStr = promptContent.toStdString();
        }
        
        // 輸出完整的 prompt 內容用於調試
        qDebug().noquote() << "LocalDocs: === Full Prompt (before LLM call) ===";
        qDebug().noquote() << QString::fromStdString(promptStr);
        qDebug().noquote() << "LocalDocs: === End of Full Prompt ===";
        
        // 調用模型處理
        QByteArray response;
        bool stop = false;
        
        auto handleResponse = [&response, &stop](LLModel::Token token, std::string_view piece) -> bool {
            Q_UNUSED(token)
            if (!stop) {
                response.append(QByteArray::fromRawData(piece.data(), piece.size()));
            }
            return !stop;
        };
        
        auto promptCallback = [](std::span<const LLModel::Token> batch, bool cached) -> bool {
            Q_UNUSED(batch)
            Q_UNUSED(cached)
            return true;
        };
        
        qDebug() << "LocalDocs: Calling LLM with document content (length:" << promptStr.length() << ", isAPIModel:" << isAPIModelForPrompt << ")";
        
        // 將阻塞的 LLM 調用移到工作線程，避免阻塞主線程
        // 注意：需要捕獲必要的變數，並確保線程安全
        LLModelStore *storePtr = store;
        Database *databasePtr = m_database;
        QString collectionCopy = collection;
        bool needReleaseCopy = needReleaseToStore;
        LLModel::PromptContext ctxCopy = ctx;  // 複製 ctx，因為它需要在 lambda 中使用
        
        QThread *workerThread = QThread::create([storePtr, databasePtr, collectionCopy, needReleaseCopy, ctxCopy, modelInfo = std::move(modelInfo), promptStr = std::move(promptStr)]() mutable {
            QByteArray threadResponse;
            bool threadStop = false;
            
            auto threadHandleResponse = [&threadResponse, &threadStop](LLModel::Token token, std::string_view piece) -> bool {
                Q_UNUSED(token)
                if (!threadStop) {
                    threadResponse.append(QByteArray::fromRawData(piece.data(), piece.size()));
                }
                return !threadStop;
            };
            
            auto threadPromptCallback = [](std::span<const LLModel::Token> batch, bool cached) -> bool {
                Q_UNUSED(batch)
                Q_UNUSED(cached)
                return true;
            };
            
            try {
                modelInfo.model->prompt(std::string_view(promptStr), threadPromptCallback, threadHandleResponse, ctxCopy);
                qDebug() << "LocalDocs: LLM prompt call completed";
            } catch (const std::exception &e) {
                qWarning() << "LocalDocs: Exception during LLM prompt call:" << e.what();
                // 使用 QMetaObject::invokeMethod 在主線程中釋放模型和更新計數
                QMetaObject::invokeMethod(QCoreApplication::instance(), [storePtr, modelInfo = std::move(modelInfo), collectionCopy, databasePtr]() mutable {
                    if (modelInfo.model) {
                        storePtr->releaseModel(std::move(modelInfo));
                    }
                    databasePtr->decrementLLMCallCount(collectionCopy);
                }, Qt::QueuedConnection);
                return;
            }
            
            // 輸出完整的 LLM response 用於調試
            QString responseStr = QString::fromUtf8(threadResponse);
            qDebug() << "LocalDocs: LLM response length:" << threadResponse.length();
            qDebug().noquote() << "LocalDocs: === Full LLM Response ===";
            qDebug().noquote() << responseStr;
            qDebug().noquote() << "LocalDocs: === End of Full LLM Response ===";
            
            // 使用 QMetaObject::invokeMethod 在主線程中釋放模型和更新計數
            QMetaObject::invokeMethod(QCoreApplication::instance(), [storePtr, modelInfo = std::move(modelInfo), collectionCopy, databasePtr, needReleaseCopy]() mutable {
                // 釋放模型
                if (needReleaseCopy) {
                    // 本地模型：釋放回 store
                    storePtr->releaseModel(std::move(modelInfo));
                    qDebug() << "LocalDocs: Model released back to store";
                } else {
                    // API 模型：直接刪除對象即可（不需要通過 store）
                    qDebug() << "LocalDocs: API model finished, cleaning up";
                    modelInfo.model.reset();
                }
                
                // 減少 LLM 調用計數（允許 UI 顯示 Ready，如果 embedding 也完成了）
                databasePtr->decrementLLMCallCount(collectionCopy);
            }, Qt::QueuedConnection);
        });
        
        // 連接線程完成信號，自動清理線程對象
        connect(workerThread, &QThread::finished, workerThread, &QThread::deleteLater);
        
        // 啟動工作線程
        workerThread->start();
        
    } catch (const std::exception &e) {
        qWarning() << "LocalDocs: Error processing document with LLM:" << e.what();
        // 錯誤情況下也要減少 LLM 調用計數
        m_database->decrementLLMCallCount(collection);
    } catch (...) {
        qWarning() << "LocalDocs: Unknown error processing document with LLM";
        // 錯誤情況下也要減少 LLM 調用計數
        m_database->decrementLLMCallCount(collection);
    }
}
