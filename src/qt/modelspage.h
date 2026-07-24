// Copyright (c) 2024 The TensorCash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_MODELSPAGE_H
#define BITCOIN_QT_MODELSPAGE_H

#include <QList>
#include <QPair>
#include <QPointer>
#include <QStringList>
#include <QWidget>
#include <QTabWidget>
#include <QTimer>
#include <QHash>
#include <QSet>
#include <univalue.h>
#include <atomic>
#include <memory>

class WalletModel;
class ClientModel;
class PlatformStyle;

enum class SyncType;
enum class SynchronizationState;

QT_BEGIN_NAMESPACE
class QComboBox;
class QLineEdit;
class QPushButton;
class QTableWidget;
class QLabel;
class QSpinBox;
class QCheckBox;
class QProgressBar;
class QGroupBox;
class QTextEdit;
QT_END_NAMESPACE

/** AI Models page for model registration and visualization */
class ModelsPage : public QWidget
{
    Q_OBJECT

public:
    explicit ModelsPage(const PlatformStyle* platformStyle, QWidget* parent = nullptr);
    ~ModelsPage();

    void setWalletModel(WalletModel* walletModel);
    void setClientModel(ClientModel* clientModel);

    // Off-thread discussion-scope fetch result. Network-scoped data shared
    // across wallets through the process-wide DiscussionScopesCoordinator in
    // modelspage.cpp, which owns the in-flight/pending/cache state and
    // broadcasts completions to every live ModelsPage.
    struct DiscussionScopesFetchResult {
        struct Scope {
            QString scopeType;
            QString scopeId;
            QString preview;
            QString modelIdentifier;
            quint64 postCount{0};
        };
        bool success{false};
        bool onlyLiveVerified{false};
        QList<Scope> scopes;
        // model_hash -> "model_name@commit" pairs from getmodelslist
        QList<QPair<QString, QString>> modelAliases;
    };
    // GUI-thread render of a scopes result; called by the coordinator on cache
    // hits and completion broadcasts.
    void applyDiscussionScopes(const DiscussionScopesFetchResult& result);
    // Labeled scope keys ("type:id" -> label) of a result; the coordinator
    // diffs them against its per-filter baselines for new-discussion detection.
    QHash<QString, QString> labeledDiscussionScopes(const DiscussionScopesFetchResult& result) const;
    // Emits the (single, process-wide) "New discussions" notification; called
    // by the coordinator on exactly one page per discovery.
    void emitNewDiscussions(const QStringList& newDiscussionLabels);
    // Current "hide scopes without live verified" filter state; the coordinator
    // keys its per-filter state on it and matches broadcasts to pages with it.
    bool discussionScopesOnlyLiveFilter() const;

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

public Q_SLOTS:
    void refreshMyDeposits();
    void refreshRegistry();
    void openDiscussionThread(const QString& scopeType, const QString& scopeHash, const QString& modelIdentifier = QString());

Q_SIGNALS:
    void message(const QString& title, const QString& message, unsigned int style);

private:
    // QPointer auto-nulls on WalletModel destruction so the discussion poll
    // and model-reviews timer survive wallet unload / app shutdown.
    QPointer<WalletModel> walletModel;
    ClientModel* clientModel{nullptr};
    const PlatformStyle* m_platform_style;

    // Main tab widget
    QTabWidget* tabWidget{nullptr};

    // ===== REGISTER TAB =====
    QWidget* registerTab{nullptr};
    QLineEdit* regNameEdit{nullptr};
    QLineEdit* regCommitEdit{nullptr};
    QLineEdit* regDifficultyEdit{nullptr};
    QLineEdit* regCidEdit{nullptr};
    QTextEdit* regExtraEdit{nullptr};
    QPushButton* regValidateButton{nullptr};
    QPushButton* regCreateButton{nullptr};
    QPushButton* regHowItWorksButton{nullptr};
    QTextEdit* regStatusText{nullptr};
    QLabel* regDisclaimerLabel{nullptr};
    bool regDepositValidated{false};
    bool regHowItWorksShownOnce{false};

    // ===== COMMIT TAB =====
    QWidget* commitTab{nullptr};
    QTableWidget* commitDepositTable{nullptr};
    QPushButton* commitRefreshButton{nullptr};
    QPushButton* commitCreateButton{nullptr};
    QLabel* commitSelectedSummaryLabel{nullptr};
    QTextEdit* commitStatusText{nullptr};
    QString selectedDepositTxid;
    int selectedDepositVout{-1};
    QString selectedDepositModelHash;
    bool commitFlowInProgress{false};

    // ===== MY DEPOSITS TAB =====
    QWidget* myDepositsTab{nullptr};
    QTableWidget* myDepositsTable{nullptr};
    QPushButton* myDepositsRefreshButton{nullptr};
    QPushButton* myDepositsReclaimButton{nullptr};
    QComboBox* myDepositsFilterCombo{nullptr};
    QLineEdit* myDepositsSearchEdit{nullptr};
    QTextEdit* myDepositsDetailsText{nullptr};
    QString myDepositsSelectedModelHash;
    bool myDepositsSelectedReclaimAllowed{false};

    // ===== REGISTRY TAB =====
    QWidget* registryTab{nullptr};
    QTableWidget* registryTable{nullptr};
    QPushButton* registryRefreshButton{nullptr};
    QComboBox* registryFilterCombo{nullptr};
    QLineEdit* registrySearchEdit{nullptr};
    QLabel* registryCurrentHeightLabel{nullptr};
    QTextEdit* registryDetailsText{nullptr};
    QPushButton* registryBurnButton{nullptr};
    QString registrySelectedModelHash;
    bool registrySelectedBurnReady{false};
    int registrySelectedBurnAllowedHeight{0};

    // ===== CHALLENGE TAB =====
    QWidget* challengeTab{nullptr};
    QTableWidget* challengeTable{nullptr};
    QPushButton* challengeRefreshButton{nullptr};
    QLabel* challengeCountLabel{nullptr};
    QTableWidget* challengeBlocksTable{nullptr};
    QPushButton* challengeLoadBlocksButton{nullptr};
    QPushButton* challengeCreateButton{nullptr};
    QPushButton* challengeSeveralCommitButton{nullptr};
    QPushButton* challengeHowItWorksButton{nullptr};
    QLabel* challengeStatusLabel{nullptr};
    QLabel* challengeDepositLabel{nullptr};
    QLabel* challengeActionLabel{nullptr};
    QLabel* challengeBlocksLabel{nullptr};
    QString challengeSelectedIdentifier;
    QString challengeSelectedHash;
    QString challengeSelectedBlockHash;
    QString challengeSelectedModelDepositTxid;
    int challengeSelectedModelDepositVout{-1};
    QString challengeSelectedDepositTxid;
    int challengeSelectedDepositVout{0};
    bool challengeHowItWorksShownOnce{false};

    // ===== DISCUSSION TAB =====
    QWidget* discussionTab{nullptr};
    QComboBox* discScopeTypeCombo{nullptr};
    QLineEdit* discModelIdentifierEdit{nullptr};
    QLineEdit* discScopeIdEdit{nullptr};
    QTableWidget* discPostsTable{nullptr};
    QPushButton* discRefreshButton{nullptr};
    QTextEdit* discComposeEdit{nullptr};
    QPushButton* discPostButton{nullptr};
    QSpinBox* discMinStakeSpin{nullptr};
    QCheckBox* discHideUnverifiedCheck{nullptr};
    QCheckBox* discHideExpiredCheck{nullptr};
    QLabel* discStatusLabel{nullptr};
    QTimer* discRefreshTimer{nullptr};
    QTimer* discScopesRefreshTimer{nullptr};
    QComboBox* discRecentScopesCombo{nullptr};
    QComboBox* discActiveScopesCombo{nullptr};
    QPushButton* discActiveScopesRefreshButton{nullptr};
    QCheckBox* discHideScopesWithoutLiveVerifiedCheck{nullptr};
    bool discBbInitialized{false};
    bool discSyncingDiscussionFields{false};
    bool discScopeAliasesLoaded{false};
    QHash<QString, QString> discScopeAliases;

    // Off-thread discussion-posts fetch (cosign.discussion_list, plus the
    // init_bb fallback). Per-page, unlike the scopes fetch: it is bound to this
    // page's selected scope and captures this page's WalletModel. Coalescing
    // flags are GUI-thread-only (mirrors TradeBoardTab); the atomic body counter
    // backs the destructor's bounded wait so no worker body outlives the wallet.
    struct DiscussionListFetchResult {
        bool success{false};
        QString errorText;   // status-label text when !success
        bool bbInitialized{false};
        QString scopeType;
        QString scopeId;
        UniValue result;
    };
    void dispatchDiscussionListFetch(const QString& scopeType, const QString& scopeId, bool force);
    void renderDiscussionPosts(const DiscussionListFetchResult& result);
    bool m_discListInFlight{false};
    bool m_discListPending{false};
    bool m_discListPendingForce{false};
    // Guards against a second cosign.discussion_post while one is in flight:
    // the disabled button alone is not enough because typing re-runs
    // onDiscussionScopeChanged(), which recomputes the button state.
    bool m_discPostInFlight{false};
    std::atomic<int> m_inflightBodies{0};

    // Timer for maturity countdown updates
    QTimer* maturityTimer{nullptr};

    // UI setup methods
    void setupRegisterTab();
    void setupCommitTab();
    void setupMyDepositsTab();
    void setupRegistryTab();
    void setupChallengeTab();
    void setupDiscussionTab();

    // Helper methods
    void validateDepositFields();
    void resetRegisterValidationState(bool clearStatus = false);
    bool parseDepositTransaction(const QString& txid, int targetVout, QString& modelHash, QString& modelName,
                                   QString& modelCommit, int64_t& difficulty, QString& cid,
                                   QString& extra, int& vout, QString& depositAmount);
    void updateMaturityCountdowns();
    bool waitForBlockAdvance(int startingHeight, int timeoutMs, const QString& phaseLabel);
    bool waitForTxConfirmation(const QString& txid, const std::string& walletName, int minConfirmations, int timeoutMs, const QString& phaseLabel);
    void runModelCommitFundingFlow(unsigned int txCount, const QString& flowLabel);
    void runChallengeCommitFundingFlow(unsigned int txCount, const QString& flowLabel);
    void loadDiscussionScopeAliases(bool force = false);
    QString discussionScopeAliasKey(const QString& scopeType, const QString& scopeId) const;
    QString lookupDiscussionScopeAlias(const QString& scopeType, const QString& scopeId) const;
    void rememberDiscussionScopeAlias(const QString& scopeType, const QString& scopeId, const QString& alias);
    QString buildDiscussionScopeLabel(const QString& scopeType, const QString& scopeId, uint64_t postCount, const QString& preview) const;
    void syncDiscussionIdentifierFromScope();
    void syncDiscussionScopeFromIdentifier();
    bool discussionChallengeScopeExists(const QString& scopeId) const;
    QString getStatusString(int status) const;
    QString getStatusColor(int status) const;
    int getCurrentHeight() const;
    void showError(const QString& message);
    void showSuccess(const QString& message);
    void refreshChallengeList();
    void refreshChallengeBlocks();

private Q_SLOTS:
    // Register tab slots
    void onRegisterFieldChanged();
    void onValidateDepositFields();
    void onCreateDeposit();
    void onRegisterHowItWorks();
    void onTabChanged(int index);

    // Commit tab slots
    void onCommitRefresh();
    void onCommitDepositSelected(int row, int column);
    void onCreateCommit();
    // My Deposits tab slots
    void onMyDepositsRefresh();
    void onMyDepositsFilterChanged(int index);
    void onMyDepositSelected(int row, int column);
    void onMyDepositReclaim();

    // Registry tab slots
    void onRegistryRefresh();
    void onRegistryFilterChanged(int index);
    void onRegistrySearchChanged();
    void onRegistryModelSelected(int row, int column);
    void onRegistryBurn();

    // Challenge tab slots
    void onChallengeRefresh();
    void onChallengeModelSelected(int row, int column);
    void onChallengeLoadBlocks();
    void onChallengeBlockSelected(int row, int column);
    void onChallengeCreate();
    void onChallengeCreateSeveralCommits();
    void onChallengeHowItWorks();

    // Discussion tab slots
    void onDiscussionRefresh(bool force = false);
    void onDiscussionPost();
    void onDiscussionTypeChanged(int index);
    void onDiscussionScopeChanged();
    void onDiscussionScopeIdEdited(const QString& text);
    void onDiscussionModelIdentifierChanged(const QString& text);
    void onDiscussionComposeChanged();
    void onDiscussionAutoRefresh();
    void onDiscussionHowItWorks();
    void onDiscussionRecentScopeSelected(int index);
    void onDiscussionActiveScopeSelected(int index);
    void onDiscussionLoadActiveScopes(bool force = false);
    void onDiscussionPostDoubleClicked(int row, int column);

    // Block update slot
    void onNumBlocksChanged(int count, const QDateTime& blockDate, double nVerificationProgress, SyncType header, SynchronizationState sync_state);

    // Timer slot
    void onMaturityTimerTick();
};

#endif // BITCOIN_QT_MODELSPAGE_H
