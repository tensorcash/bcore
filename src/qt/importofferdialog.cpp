// Copyright (c) 2025 The TensorCash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://opensource.org/license/mit/.

#include <qt/importofferdialog.h>
#include <qt/bridgesessionmanager.h>
#include <qt/walletmodel.h>
#include <qt/reviewofferdialog.h>
#include <qt/opencontractdialog.h>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QLabel>
#include <QComboBox>
#include <QTextEdit>
#include <QPushButton>
#include <QMessageBox>
#include <QFileDialog>
#include <QClipboard>
#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>

ImportOfferDialog::ImportOfferDialog(WalletModel* model, BridgeSessionManager* sessionMgr, QWidget* parent)
    : QDialog(parent),
      walletModel(model),
      sessionManager(sessionMgr)
{
    setWindowTitle(tr("Import Contract Offer"));
    setMinimumWidth(600);
    setMinimumHeight(400);

    setupUI();
}

ImportOfferDialog::~ImportOfferDialog()
{
}

void ImportOfferDialog::setupUI()
{
    QVBoxLayout* mainLayout = new QVBoxLayout(this);

    // Title
    QLabel* titleLabel = new QLabel(tr("<h2>Import Contract Offer</h2>"), this);
    mainLayout->addWidget(titleLabel);

    QLabel* subtitleLabel = new QLabel(
        tr("Import an offer from JSON, file, or active cosign session."), this);
    subtitleLabel->setWordWrap(true);
    mainLayout->addWidget(subtitleLabel);

    mainLayout->addSpacing(15);

    // Import method selector
    QGroupBox* methodGroup = new QGroupBox(tr("Import Method"), this);
    QVBoxLayout* methodLayout = new QVBoxLayout(methodGroup);

    methodCombo = new QComboBox(this);
    methodCombo->addItem(tr("Manual JSON Paste"), "manual");
    methodCombo->addItem(tr("Load from File"), "file");
    methodCombo->addItem(tr("Receive from Cosign Session"), "cosign");
    methodLayout->addWidget(methodCombo);

    connect(methodCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, &ImportOfferDialog::onImportMethodChanged);

    methodGroup->setLayout(methodLayout);
    mainLayout->addWidget(methodGroup);

    mainLayout->addSpacing(10);

    // Offer JSON input area
    QGroupBox* offerGroup = new QGroupBox(tr("Offer JSON"), this);
    QVBoxLayout* offerLayout = new QVBoxLayout(offerGroup);

    offerJsonEdit = new QTextEdit(this);
    offerJsonEdit->setPlaceholderText(tr("Paste offer JSON here..."));
    offerJsonEdit->setMinimumHeight(200);
    offerLayout->addWidget(offerJsonEdit);

    // Action buttons for JSON input
    QHBoxLayout* jsonActionsLayout = new QHBoxLayout();

    pasteButton = new QPushButton(tr("Paste from Clipboard"), this);
    connect(pasteButton, &QPushButton::clicked, this, &ImportOfferDialog::onPasteFromClipboard);
    jsonActionsLayout->addWidget(pasteButton);

    loadFileButton = new QPushButton(tr("Load from File"), this);
    connect(loadFileButton, &QPushButton::clicked, this, &ImportOfferDialog::onLoadFromFile);
    jsonActionsLayout->addWidget(loadFileButton);

    jsonActionsLayout->addStretch();

    offerLayout->addLayout(jsonActionsLayout);

    offerGroup->setLayout(offerLayout);
    mainLayout->addWidget(offerGroup);

    // Cosign session selector (hidden by default)
    QGroupBox* cosignGroup = new QGroupBox(tr("Cosign Session"), this);
    cosignGroup->setObjectName("cosignGroup");
    cosignGroup->hide();
    QHBoxLayout* cosignLayout = new QHBoxLayout(cosignGroup);

    sessionCombo = new QComboBox(this);
    sessionCombo->addItem(tr("(No active sessions)"), "");
    cosignLayout->addWidget(sessionCombo);

    receiveButton = new QPushButton(tr("Receive Offer"), this);
    connect(receiveButton, &QPushButton::clicked, this, &ImportOfferDialog::onReceiveFromSession);
    cosignLayout->addWidget(receiveButton);

    cosignGroup->setLayout(cosignLayout);
    mainLayout->addWidget(cosignGroup);

    mainLayout->addSpacing(10);

    // Status label
    statusLabel = new QLabel(this);
    statusLabel->setWordWrap(true);
    statusLabel->setStyleSheet("QLabel { color: #666; font-style: italic; }");
    statusLabel->hide();
    mainLayout->addWidget(statusLabel);

    mainLayout->addStretch();

    // Dialog buttons
    QHBoxLayout* buttonLayout = new QHBoxLayout();

    importButton = new QPushButton(tr("Import && Review"), this);
    importButton->setDefault(true);
    importButton->setEnabled(false);
    connect(importButton, &QPushButton::clicked, this, &ImportOfferDialog::onImport);
    buttonLayout->addWidget(importButton);

    cancelButton = new QPushButton(tr("Cancel"), this);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    buttonLayout->addWidget(cancelButton);

    buttonLayout->addStretch();

    mainLayout->addLayout(buttonLayout);

    setLayout(mainLayout);

    // Enable import button when text is entered
    connect(offerJsonEdit, &QTextEdit::textChanged, [this]() {
        importButton->setEnabled(!offerJsonEdit->toPlainText().trimmed().isEmpty());
    });
}

void ImportOfferDialog::onImportMethodChanged(int index)
{
    QString method = methodCombo->currentData().toString();

    // Show/hide relevant UI elements
    QGroupBox* cosignGroup = findChild<QGroupBox*>("cosignGroup");

    if (method == "manual" || method == "file") {
        offerJsonEdit->setEnabled(true);
        pasteButton->setVisible(method == "manual");
        loadFileButton->setVisible(method == "file");
        if (cosignGroup) cosignGroup->hide();
    } else if (method == "cosign") {
        offerJsonEdit->setEnabled(false);
        pasteButton->hide();
        loadFileButton->hide();
        if (cosignGroup) {
            cosignGroup->show();
            // Populate active sessions from session manager
            sessionCombo->clear();

            if (sessionManager) {
                QList<BridgeSessionManager::SessionInfo> sessions = sessionManager->getHandshakeCompleteSessions();
                if (sessions.isEmpty()) {
                    sessionCombo->addItem(tr("(No sessions with completed handshakes)"), "");
                } else {
                    for (const BridgeSessionManager::SessionInfo& session : sessions) {
                        QString label = tr("%1 - SAS: %2")
                            .arg(session.session_id.left(12) + "...")
                            .arg(session.sas);
                        sessionCombo->addItem(label, session.session_id);
                    }
                }
            } else {
                sessionCombo->addItem(tr("(Session manager not available)"), "");
            }
        }
    }
}

void ImportOfferDialog::onPasteFromClipboard()
{
    QClipboard* clipboard = QApplication::clipboard();
    QString text = clipboard->text();

    if (text.isEmpty()) {
        QMessageBox::information(this, tr("Clipboard Empty"),
            tr("Clipboard does not contain any text."));
        return;
    }

    offerJsonEdit->setPlainText(text);
    statusLabel->setText(tr("Pasted from clipboard. Click 'Import && Review' to validate."));
    statusLabel->setStyleSheet("QLabel { color: #1976d2; font-style: italic; }");
    statusLabel->show();
}

void ImportOfferDialog::onLoadFromFile()
{
    QString fileName = QFileDialog::getOpenFileName(this,
        tr("Load Offer JSON"), QString(), tr("JSON Files (*.json);;All Files (*)"));

    if (fileName.isEmpty()) {
        return;
    }

    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::critical(this, tr("File Error"),
            tr("Could not open file:\n%1").arg(fileName));
        return;
    }

    QTextStream in(&file);
    QString json = in.readAll();
    file.close();

    offerJsonEdit->setPlainText(json);
    statusLabel->setText(tr("Loaded from file: %1").arg(fileName));
    statusLabel->setStyleSheet("QLabel { color: #1976d2; font-style: italic; }");
    statusLabel->show();
}

void ImportOfferDialog::onReceiveFromSession()
{
    QString sessionId = sessionCombo->currentData().toString();

    if (sessionId.isEmpty()) {
        QMessageBox::information(this, tr("No Session Selected"),
            tr("Please select an active cosign session."));
        return;
    }

    if (!walletModel) {
        QMessageBox::critical(this, tr("Error"), tr("Wallet model not available"));
        return;
    }

    // Call cosign.recv to get offer
    statusLabel->setText(tr("Waiting for offer from session %1...").arg(sessionId.left(16) + "..."));
    statusLabel->setStyleSheet("QLabel { color: #1976d2; font-style: italic; }");
    statusLabel->show();

    WalletModel::CosignRecvResult result = walletModel->cosignRecv(sessionId, 30000); // 30 sec timeout

    if (!result.success) {
        statusLabel->setText(tr("Failed to receive offer: %1").arg(result.error));
        statusLabel->setStyleSheet("QLabel { color: #d32f2f; }");
        return;
    }

    if (result.payload_json.isEmpty()) {
        statusLabel->setText(tr("No message received (timeout or no pending messages)"));
        statusLabel->setStyleSheet("QLabel { color: #f57c00; }");
        return;
    }

    offerJsonEdit->setPlainText(result.payload_json);
    statusLabel->setText(tr("Received offer via cosign session. Click 'Import && Review' to validate."));
    statusLabel->setStyleSheet("QLabel { color: #388e3c; }");
}

void ImportOfferDialog::onImport()
{
    QString json = offerJsonEdit->toPlainText().trimmed();

    if (json.isEmpty()) {
        QMessageBox::warning(this, tr("Empty Input"),
            tr("Please paste or load an offer JSON."));
        return;
    }

    if (validateAndImportOffer(json)) {
        accept();
    }
}

bool ImportOfferDialog::validateAndImportOffer(const QString& json)
{
    if (!walletModel) {
        QMessageBox::critical(this, tr("Error"), tr("Wallet model not available"));
        return false;
    }

    // Parse JSON to determine contract type
    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
    if (doc.isNull() || !doc.isObject()) {
        QMessageBox::critical(this, tr("Invalid JSON"),
            tr("The provided text is not valid JSON."));
        return false;
    }

    QJsonObject obj = doc.object();
    QString type = obj.value("contract_type").toString();

    if (type.isEmpty()) {
        QMessageBox::critical(this, tr("Invalid Offer"),
            tr("Offer JSON does not contain a 'contract_type' field."));
        return false;
    }

    // Call appropriate import RPC based on contract type
    if (type == "repo") {
        WalletModel::RepoImportOfferResult result = walletModel->repoImportOffer(json);

        if (!result.success) {
            QMessageBox::critical(this, tr("Import Failed"),
                tr("Failed to import repo offer:\n\n%1").arg(result.error));
            return false;
        }

        offerId = result.offer_id;
        contractType = "repo";
        offerJson = json;

        // Launch ReviewOfferDialog for acceptance
        QVariantMap offerData = obj.toVariantMap();
        ReviewOfferDialog reviewDialog(walletModel, offerId, offerData, this);

        if (reviewDialog.exec() == QDialog::Accepted && reviewDialog.wasAccepted()) {
            // Offer was accepted, now launch OpenContractDialog
            OpenContractDialog openDialog(walletModel, offerId, contractType, offerData, this);

            if (openDialog.exec() == QDialog::Accepted && openDialog.wasOpened()) {
                QMessageBox::information(this, tr("Success"),
                    tr("Contract opened successfully!\n\nTransaction ID: %1")
                    .arg(openDialog.getOpeningTxId()));
            }
        }

    } else if (type == "forward") {
        // Phase 6: Forward import
        QMessageBox::information(this, tr("Coming Soon"),
            tr("Forward contract import will be available in Phase 6."));
        return false;

    } else if (type == "spot") {
        // Phase 7: Spot import
        QMessageBox::information(this, tr("Coming Soon"),
            tr("Spot contract import will be available in Phase 7."));
        return false;

    } else {
        QMessageBox::critical(this, tr("Unknown Contract Type"),
            tr("Unknown contract type: %1").arg(type));
        return false;
    }

    return true;
}
