/* GUI for ollama
 * 
 * Build:
 * $(pkg-config --variable=libexecdir Qt6Core)/moc main.cpp -o main.moc && g++ -std=c++20 -fPIC main.cpp -o ollama_gui   $(pkg-config --cflags Qt6Widgets Qt6Network | sed 's/-I/-isystem /g')   $(pkg-config --libs Qt6Widgets Qt6Network)   -Wall -Wextra -Wpedantic -Werror   -Wconversion -Wsign-conversion -Wshadow -Wnon-virtual-dtor   -Wold-style-cast -Woverloaded-virtual -Wnull-dereference   -Wdouble-promotion -Wformat=2
 * 
 *
 * Usage: ./ollama_gui [model_name]   (skips the picker)
 */

#include <QApplication>
#include <QWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QTextEdit>
#include <QLineEdit>
#include <QPushButton>
#include <QLabel>
#include <QDialog>
#include <QListWidget>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QScrollBar>
#include <QDialogButtonBox>
#include <QTextCursor>

#include <string_view>

// -- Constants --------------------------------------------------------------

namespace OllamaGui {
	
	inline constexpr std::string_view kDefaultModel  = "smollm:360m";
	inline constexpr std::string_view kApiTagsUrl    = "http://localhost:11434/api/tags";
	inline constexpr std::string_view kApiGenerateUrl= "http://localhost:11434/api/generate";
	inline constexpr int kMinPickerWidth = 360;
	inline constexpr int kWindowWidth    = 520;
	inline constexpr int kWindowHeight   = 640;
	inline constexpr int kSwitchBtnWidth = 120;
	
} // namespace OllamaGui

// -- Model picker dialog ----------------------------------------------------

class ModelPickerDialog final : public QDialog {
	Q_OBJECT
	
public:
	explicit ModelPickerDialog(QWidget *parent = nullptr)
	: QDialog{parent}
	, m_statusLabel{new QLabel{tr("Fetching models from Ollama\xe2\x80\xa6"), this}}
	, m_listWidget{new QListWidget{this}}
	, m_manualEntry{new QLineEdit{this}}
	{
		setWindowTitle(tr("Select model"));
		setMinimumWidth(OllamaGui::kMinPickerWidth);
		
		auto *layout = new QVBoxLayout{this};
		
		m_statusLabel->setStyleSheet(QStringLiteral("color: gray; font-size: 13px;"));
		layout->addWidget(m_statusLabel);
		
		m_listWidget->setAlternatingRowColors(true);
		layout->addWidget(m_listWidget);
		
		m_manualEntry->setPlaceholderText(
			tr("Or type a model name manually\xe2\x80\xa6"));
		layout->addWidget(m_manualEntry);
		
		auto *buttons = new QDialogButtonBox{
			QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this};
			layout->addWidget(buttons);
			
			connect(buttons,      &QDialogButtonBox::accepted,
					this,         &ModelPickerDialog::accept);
			connect(buttons,      &QDialogButtonBox::rejected,
					this,         &QDialog::reject);
			connect(m_listWidget, &QListWidget::itemDoubleClicked,
					this,         &ModelPickerDialog::accept);
			connect(m_listWidget, &QListWidget::currentTextChanged,
					m_manualEntry,&QLineEdit::setText);
			connect(m_manualEntry,&QLineEdit::textChanged,
					this,         &ModelPickerDialog::onManualTextChanged);
			
			fetchModels();
	}
	
	[[nodiscard]] QString selectedModel() const
	{
		const QString m = m_manualEntry->text().trimmed();
		return m.isEmpty()
		? QString::fromLatin1(OllamaGui::kDefaultModel.data(),
							  static_cast<qsizetype>(OllamaGui::kDefaultModel.size()))
		: m;
	}
	
private slots:
	void onManualTextChanged(const QString &text)
	{
		for (int i = 0; i < m_listWidget->count(); ++i) {
			if (m_listWidget->item(i)->text() == text) {
				m_listWidget->setCurrentRow(i);
				return;
			}
		}
		m_listWidget->clearSelection();
	}
	
	void onTagsReplyFinished()
	{
		auto *reply = qobject_cast<QNetworkReply *>(sender());
		if (!reply) return;
		reply->deleteLater();
		
		if (reply->error() != QNetworkReply::NoError) {
			m_statusLabel->setText(
				tr("Could not reach Ollama \xe2\x80\x94 type a model name below."));
			m_statusLabel->setStyleSheet(
				QStringLiteral("color: #c0392b; font-size: 13px;"));
			return;
		}
		
		const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
		const QJsonArray models = doc.object()[QStringLiteral("models")].toArray();
		
		if (models.isEmpty()) {
			m_statusLabel->setText(
				tr("No models found. Pull one with: ollama pull <name>"));
			m_statusLabel->setStyleSheet(
				QStringLiteral("color: #e67e22; font-size: 13px;"));
			return;
		}
		
		m_listWidget->clear();
		for (const QJsonValue &v : models) {
			const QString name = v.toObject()[QStringLiteral("name")].toString();
			if (!name.isEmpty())
				m_listWidget->addItem(name);
		}
		
		m_listWidget->setCurrentRow(0);
		m_statusLabel->setText(
			tr("%n model(s) available", nullptr, static_cast<int>(models.size())));
		m_statusLabel->setStyleSheet(
			QStringLiteral("color: gray; font-size: 13px;"));
	}
	
private:
	void fetchModels()
	{
		auto *manager = new QNetworkAccessManager{this};
		const QNetworkRequest req{
			QUrl{QString::fromLatin1(OllamaGui::kApiTagsUrl.data(),
				static_cast<qsizetype>(OllamaGui::kApiTagsUrl.size()))}};
				QNetworkReply *reply = manager->get(req);
				connect(reply, &QNetworkReply::finished,
						this,  &ModelPickerDialog::onTagsReplyFinished);
	}
	
	QLabel      *m_statusLabel;
	QListWidget *m_listWidget;
	QLineEdit   *m_manualEntry;
};

// -- Chat window ------------------------------------------------------------

class ChatWindow final : public QWidget {
	Q_OBJECT
	
public:
	explicit ChatWindow(QString model, QWidget *parent = nullptr)
	: QWidget{parent}
	, m_currentModel{std::move(model)}
	, m_chatBox{new QTextEdit{this}}
	, m_inputField{new QLineEdit{this}}
	, m_sendButton{new QPushButton{tr("Send"), this}}
	, m_modelLabel{new QLabel{this}}
	, m_manager{new QNetworkAccessManager{this}}
	{
		setWindowTitle(QStringLiteral("Ollama \xe2\x80\x94 ") + m_currentModel);
		resize(OllamaGui::kWindowWidth, OllamaGui::kWindowHeight);
		
		auto *mainLayout = new QVBoxLayout{this};
		
		// Header bar
		auto *headerLayout = new QHBoxLayout{};
		updateModelLabel();
		auto *switchBtn = new QPushButton{tr("Switch model\xe2\x80\xa6"), this};
		switchBtn->setFixedWidth(OllamaGui::kSwitchBtnWidth);
		headerLayout->addWidget(m_modelLabel);
		headerLayout->addStretch();
		headerLayout->addWidget(switchBtn);
		mainLayout->addLayout(headerLayout);
		
		m_chatBox->setReadOnly(true);
		mainLayout->addWidget(m_chatBox);
		
		auto *inputLayout = new QHBoxLayout{};
		m_inputField->setPlaceholderText(tr("Type a message\xe2\x80\xa6"));
		inputLayout->addWidget(m_inputField);
		inputLayout->addWidget(m_sendButton);
		mainLayout->addLayout(inputLayout);
		
		connect(m_sendButton, &QPushButton::clicked,
				this,         &ChatWindow::sendMessage);
		connect(m_inputField, &QLineEdit::returnPressed,
				this,         &ChatWindow::sendMessage);
		connect(switchBtn,    &QPushButton::clicked,
				this,         &ChatWindow::pickModel);
	}
	
private slots:
	void sendMessage()
	{
		const QString userText = m_inputField->text().trimmed();
		if (userText.isEmpty()) return;
		
		if (m_activeReply && m_activeReply->isRunning())
			m_activeReply->abort();
		
		m_chatBox->append(QStringLiteral("<b>You:</b> ") + userText.toHtmlEscaped());
		m_inputField->clear();
		m_sendButton->setEnabled(false);
		
		QNetworkRequest request{
			QUrl{QString::fromLatin1(OllamaGui::kApiGenerateUrl.data(),
				static_cast<qsizetype>(OllamaGui::kApiGenerateUrl.size()))}};
				request.setHeader(QNetworkRequest::ContentTypeHeader,
								  QStringLiteral("application/json"));
				
				QJsonObject json;
				json[QStringLiteral("model")]  = m_currentModel;
				json[QStringLiteral("prompt")] = userText;
				json[QStringLiteral("stream")] = true;
				
				m_activeReply = m_manager->post(request, QJsonDocument{json}.toJson());
				
				// Append bot prefix without trailing newline
				m_chatBox->append(QStringLiteral("Bot: "));
				{
					QTextCursor c = m_chatBox->textCursor();
					c.movePosition(QTextCursor::End);
					c.deletePreviousChar();
					m_chatBox->setTextCursor(c);
				}
				
				connect(m_activeReply, &QNetworkReply::readyRead,
						this,          &ChatWindow::onReadyRead);
				connect(m_activeReply, &QNetworkReply::finished,
						this,          &ChatWindow::onReplyFinished);
	}
	
	void onReadyRead()
	{
		if (!m_activeReply) return;
		while (m_activeReply->canReadLine()) {
			const QByteArray line = m_activeReply->readLine().trimmed();
			if (line.isEmpty()) continue;
			const QJsonDocument doc = QJsonDocument::fromJson(line);
			if (doc.isNull()) continue;
			const QString token =
			doc.object()[QStringLiteral("response")].toString();
			appendToken(token);
		}
	}
	
	void onReplyFinished()
	{
		if (!m_activeReply) return;
		
		// Drain any lines that arrived with the finished signal
		while (m_activeReply->canReadLine()) {
			const QByteArray line = m_activeReply->readLine().trimmed();
			if (line.isEmpty()) continue;
			const QJsonDocument doc = QJsonDocument::fromJson(line);
			if (doc.isNull()) continue;
			const QString token =
			doc.object()[QStringLiteral("response")].toString();
			if (!token.isEmpty())
				appendToken(token);
		}
		
		// Final newline after bot response
		QTextCursor cur = m_chatBox->textCursor();
		cur.movePosition(QTextCursor::End);
		cur.insertText(QStringLiteral("\n"));
		
		if (m_activeReply->error() != QNetworkReply::NoError &&
			m_activeReply->error() != QNetworkReply::OperationCanceledError)
		{
			m_chatBox->append(
				QStringLiteral("[Error: ") + m_activeReply->errorString() +
				QStringLiteral("]"));
		}
		
		m_sendButton->setEnabled(true);
		m_activeReply->deleteLater();
		m_activeReply = nullptr;
	}
	
	void pickModel()
	{
		ModelPickerDialog dlg{this};
		if (dlg.exec() != QDialog::Accepted) return;
		m_currentModel = dlg.selectedModel();
		setWindowTitle(QStringLiteral("Ollama \xe2\x80\x94 ") + m_currentModel);
		updateModelLabel();
		m_chatBox->append(
			QStringLiteral("<i>\xe2\x80\x94 switched to ") +
			m_currentModel.toHtmlEscaped() +
			QStringLiteral(" \xe2\x80\x94</i>"));
	}
	
private:
	void updateModelLabel()
	{
		m_modelLabel->setText(
			QStringLiteral("Model: <b>") +
			m_currentModel.toHtmlEscaped() +
			QStringLiteral("</b>"));
	}
	
	void appendToken(const QString &token)
	{
		QTextCursor cur = m_chatBox->textCursor();
		cur.movePosition(QTextCursor::End);
		cur.insertText(token);
		m_chatBox->verticalScrollBar()->setValue(
			m_chatBox->verticalScrollBar()->maximum());
	}
	
	QString                m_currentModel;
	QTextEdit             *m_chatBox      = nullptr;
	QLineEdit             *m_inputField   = nullptr;
	QPushButton           *m_sendButton   = nullptr;
	QLabel                *m_modelLabel   = nullptr;
	QNetworkAccessManager *m_manager      = nullptr;
	QNetworkReply         *m_activeReply  = nullptr;
};

// -- Entry point ------------------------------------------------------------

#include "main.moc"

int main(int argc, char *argv[])
{
	QApplication app{argc, argv};
	
	QString model;
	
	if (argc > 1) {
		model = QString::fromLocal8Bit(argv[1]);
	} else {
		ModelPickerDialog picker;
		if (picker.exec() != QDialog::Accepted)
			return 0;
		model = picker.selectedModel();
	}
	
	ChatWindow window{std::move(model)};
	window.show();
	
	return app.exec();
}
