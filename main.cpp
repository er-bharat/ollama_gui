/* GUI for ollama — with session sidebar
 * 
 * Build:
 * $(pkg-config --variable=libexecdir Qt6Core)/moc main.cpp -o main.moc && g++ -std=c++20 -fPIC main.cpp -o ollama_gui   $(pkg-config --cflags Qt6Widgets Qt6Network | sed 's/-I/-isystem /g')   $(pkg-config --libs Qt6Widgets Qt6Network)   -Wall -Wextra -Wpedantic -Werror   -Wconversion -Wsign-conversion -Wshadow -Wnon-virtual-dtor   -Wold-style-cast -Woverloaded-virtual -Wnull-dereference   -Wdouble-promotion -Wformat=2
 *
 * Usage: ./ollama_gui [model_name]   (skips the picker)
 *
 * Sessions are stored in ~/.ollama_gui_sessions/ as JSON files.
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
#include <QListWidgetItem>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QScrollBar>
#include <QDialogButtonBox>
#include <QTextCursor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <QDateTime>
#include <QUuid>
#include <QMenu>
#include <QInputDialog>
#include <QMessageBox>
#include <QFrame>
#include <QTimer>
#include <QTextDocument>
#include <QProcess>
#include <QTcpSocket>
#include <QThread>
#include <QSplitter>
#include <string_view>

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------

namespace OllamaGui {
	
	inline constexpr std::string_view kDefaultModel   = "smollm:360m";
	inline constexpr std::string_view kApiTagsUrl     = "http://localhost:11434/api/tags";
	inline constexpr std::string_view kApiGenerateUrl = "http://localhost:11434/api/generate";
	inline constexpr int kMinPickerWidth = 360;
	inline constexpr int kWindowWidth    = 920;
	inline constexpr int kWindowHeight   = 680;
	inline constexpr int kSidebarWidth   = 220;
	
} // namespace OllamaGui

// ---------------------------------------------------------------------------
// Session data structures
// ---------------------------------------------------------------------------

struct ChatMessage {
	QString role; // "user" or "bot"
	QString text;
};

struct Session {
	QString              id;
	QString              title;
	QString              model;
	QDateTime            updatedAt;
	QVector<ChatMessage> messages;
	
	static Session create(const QString &model)
	{
		Session s;
		s.id        = QUuid::createUuid().toString(QUuid::WithoutBraces);
		s.title     = QStringLiteral("New chat");
		s.model     = model;
		s.updatedAt = QDateTime::currentDateTime();
		return s;
	}
	
	[[nodiscard]] QJsonObject toJson() const
	{
		QJsonArray msgs;
		for (const auto &m : messages) {
			QJsonObject o;
			o[QStringLiteral("role")] = m.role;
			o[QStringLiteral("text")] = m.text;
			msgs.append(o);
		}
		QJsonObject obj;
		obj[QStringLiteral("id")]        = id;
		obj[QStringLiteral("title")]     = title;
		obj[QStringLiteral("model")]     = model;
		obj[QStringLiteral("updatedAt")] = updatedAt.toString(Qt::ISODate);
		obj[QStringLiteral("messages")]  = msgs;
		return obj;
	}
	
	static Session fromJson(const QJsonObject &obj)
	{
		Session s;
		s.id        = obj[QStringLiteral("id")].toString();
		s.title     = obj[QStringLiteral("title")].toString();
		s.model     = obj[QStringLiteral("model")].toString();
		s.updatedAt = QDateTime::fromString(
			obj[QStringLiteral("updatedAt")].toString(), Qt::ISODate);
		for (const auto &v : obj[QStringLiteral("messages")].toArray()) {
			const QJsonObject mo = v.toObject();
			ChatMessage cm;
			cm.role = mo[QStringLiteral("role")].toString();
			cm.text = mo[QStringLiteral("text")].toString();
			s.messages.append(cm);
		}
		return s;
	}
};

// ---------------------------------------------------------------------------
// SessionStore  —  load/save sessions to disk
// ---------------------------------------------------------------------------

class SessionStore {
public:
	SessionStore()
	{
		m_dir = QDir{
			QStandardPaths::writableLocation(QStandardPaths::HomeLocation)
			+ QStringLiteral("/.ollama_gui_sessions")};
			m_dir.mkpath(QStringLiteral("."));
	}
	
	[[nodiscard]] QVector<Session> loadAll() const
	{
		QVector<Session> list;
		const QStringList files =
		m_dir.entryList(QStringList{QStringLiteral("*.json")}, QDir::Files);
		for (const QString &fname : files) {
			QFile f{m_dir.filePath(fname)};
			if (!f.open(QIODevice::ReadOnly)) continue;
			const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
			if (doc.isNull() || !doc.isObject()) continue;
			list.append(Session::fromJson(doc.object()));
		}
		std::sort(list.begin(), list.end(), [](const Session &a, const Session &b) {
			return a.updatedAt > b.updatedAt;
		});
		return list;
	}
	
	void save(const Session &session) const
	{
		QFile f{m_dir.filePath(session.id + QStringLiteral(".json"))};
		if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
		f.write(QJsonDocument{session.toJson()}.toJson());
	}
	
	void remove(const QString &id)
	{
		m_dir.remove(id + QStringLiteral(".json"));
	}
	
private:
	QDir m_dir;
};

// ---------------------------------------------------------------------------
// Model picker dialog
// ---------------------------------------------------------------------------

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
		layout->setSpacing(8);
		layout->setContentsMargins(12, 12, 12, 12);
		
		m_statusLabel->setEnabled(false);
		layout->addWidget(m_statusLabel);
		
		m_listWidget->setAlternatingRowColors(true);
		layout->addWidget(m_listWidget);
		
		m_manualEntry->setPlaceholderText(
			tr("Or type a model name manually\xe2\x80\xa6"));
		layout->addWidget(m_manualEntry);
		
		auto *buttons = new QDialogButtonBox{
			QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this};
			layout->addWidget(buttons);
			
			connect(buttons,       &QDialogButtonBox::accepted,
					this,          &ModelPickerDialog::accept);
			connect(buttons,       &QDialogButtonBox::rejected,
					this,          &QDialog::reject);
			connect(m_listWidget,  &QListWidget::itemDoubleClicked,
					this,          &ModelPickerDialog::accept);
			connect(m_listWidget,  &QListWidget::currentTextChanged,
					m_manualEntry, &QLineEdit::setText);
			connect(m_manualEntry, &QLineEdit::textChanged,
					this,          &ModelPickerDialog::onManualTextChanged);
			
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
			m_statusLabel->setEnabled(false);
			return;
		}
		
		const QJsonDocument doc    = QJsonDocument::fromJson(reply->readAll());
		const QJsonArray    models = doc.object()[QStringLiteral("models")].toArray();
		
		if (models.isEmpty()) {
			m_statusLabel->setText(
				tr("No models found. Pull one with: ollama pull <name>"));
			m_statusLabel->setEnabled(false);
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
		m_statusLabel->setEnabled(false);
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

// ---------------------------------------------------------------------------
// Session sidebar
// ---------------------------------------------------------------------------

class SessionSidebar final : public QWidget {
	Q_OBJECT
	
public:
	explicit SessionSidebar(SessionStore *store, QWidget *parent = nullptr)
	: QWidget{parent}
	, m_store{store}
	, m_listWidget{new QListWidget{this}}
	{
		setFixedWidth(OllamaGui::kSidebarWidth);
		
		auto *outerLayout = new QVBoxLayout{this};
		outerLayout->setContentsMargins(0, 0, 0, 0);
		outerLayout->setSpacing(0);
		
		// -- Header bar: matches chat pane header exactly ------------------
		// Uses a plain QWidget so it naturally picks up the window background.
		auto *headerWidget = new QWidget{this};
		// Fixed height matches the chat pane's own header widget below.
		headerWidget->setFixedHeight(40);
		auto *headerLayout = new QHBoxLayout{headerWidget};
		// Left/right margins mirror the chat header (8 px each side).
		headerLayout->setContentsMargins(8, 0, 8, 0);
		headerLayout->setSpacing(0);
		
		auto *titleLabel = new QLabel{tr("Sessions"), headerWidget};
		QFont f = titleLabel->font();
		f.setBold(true);
		titleLabel->setFont(f);
		headerLayout->addWidget(titleLabel);
		
		outerLayout->addWidget(headerWidget);
		
		// -- Native horizontal separator -----------------------------------
		auto *topSep = new QFrame{this};
		topSep->setFrameShape(QFrame::HLine);
		topSep->setFrameShadow(QFrame::Sunken);
		outerLayout->addWidget(topSep);
		
		// -- New session button --------------------------------------------
		auto *btnWidget = new QWidget{this};
		auto *btnLayout = new QHBoxLayout{btnWidget};
		btnLayout->setContentsMargins(8, 6, 8, 6);
		auto *newBtn = new QPushButton{tr("New session"), btnWidget};
		btnLayout->addWidget(newBtn);
		outerLayout->addWidget(btnWidget);
		
		// -- Session list --------------------------------------------------
		m_listWidget->setAlternatingRowColors(true);
		m_listWidget->setContextMenuPolicy(Qt::CustomContextMenu);
		m_listWidget->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
		m_listWidget->setTextElideMode(Qt::ElideRight);
		m_listWidget->setResizeMode(QListView::Adjust);
		m_listWidget->setFrameShape(QFrame::NoFrame); // flush with sidebar
		outerLayout->addWidget(m_listWidget);
		
		connect(newBtn,       &QPushButton::clicked,
				this,         &SessionSidebar::onNewSession);
		connect(m_listWidget, &QListWidget::currentItemChanged,
				this,         &SessionSidebar::onItemChanged);
		connect(m_listWidget, &QListWidget::customContextMenuRequested,
				this,         &SessionSidebar::onContextMenu);
		
		reload();
	}
	
	void reload(const QString &selectId = {})
	{
		const QVector<Session> sessions = m_store->loadAll();
		
		m_listWidget->blockSignals(true);
		m_listWidget->clear();
		int selectRow = 0;
		for (int i = 0; i < sessions.size(); ++i) {
			const Session &s   = sessions[i];
			auto          *item = new QListWidgetItem{s.title};
			item->setData(Qt::UserRole, s.id);
			item->setToolTip(s.model + QStringLiteral("\n") +
			s.updatedAt.toString(QStringLiteral("yyyy-MM-dd hh:mm")));
			m_listWidget->addItem(item);
			if (!selectId.isEmpty() && s.id == selectId)
				selectRow = i;
		}
		m_listWidget->blockSignals(false);
		
		if (m_listWidget->count() > 0)
			m_listWidget->setCurrentRow(selectRow);
	}
	
	void setActiveId(const QString &id)
	{
		m_listWidget->blockSignals(true);
		for (int i = 0; i < m_listWidget->count(); ++i) {
			if (m_listWidget->item(i)->data(Qt::UserRole).toString() == id) {
				m_listWidget->setCurrentRow(i);
				break;
			}
		}
		m_listWidget->blockSignals(false);
	}
	
signals:
	void sessionSelected(const QString &id);
	void newSessionRequested();
	void renameRequested(const QString &id, const QString &newTitle);
	void deleteRequested(const QString &id);
	
private slots:
	void onNewSession() { emit newSessionRequested(); }
	
	void onItemChanged(QListWidgetItem *current, QListWidgetItem * /*previous*/)
	{
		if (!current) return;
		emit sessionSelected(current->data(Qt::UserRole).toString());
	}
	
	void onContextMenu(const QPoint &pos)
	{
		auto *item = m_listWidget->itemAt(pos);
		if (!item) return;
		const QString id = item->data(Qt::UserRole).toString();
		
		QMenu    menu{this};
		QAction *renameAct = menu.addAction(tr("Rename\xe2\x80\xa6"));
		QAction *deleteAct = menu.addAction(tr("Delete"));
		deleteAct->setIcon(style()->standardIcon(QStyle::SP_TrashIcon));
		
		QAction *chosen = menu.exec(m_listWidget->mapToGlobal(pos));
		if (chosen == renameAct) {
			bool          ok       = false;
			const QString newTitle = QInputDialog::getText(
				this, tr("Rename session"), tr("New name:"),
														   QLineEdit::Normal, item->text(), &ok);
			if (ok && !newTitle.trimmed().isEmpty())
				emit renameRequested(id, newTitle.trimmed());
		} else if (chosen == deleteAct) {
			const auto btn = QMessageBox::question(
				this, tr("Delete session"),
												   tr("Delete \"%1\"? This cannot be undone.").arg(item->text()));
			if (btn == QMessageBox::Yes)
				emit deleteRequested(id);
		}
	}
	
private:
	SessionStore *m_store;
	QListWidget  *m_listWidget;
};

// ---------------------------------------------------------------------------
// Chat pane  —  shows one session
// ---------------------------------------------------------------------------

class ChatPane final : public QWidget {
	Q_OBJECT
	
public:
	explicit ChatPane(QWidget *parent = nullptr)
	: QWidget{parent}
	, m_chatBox{new QTextEdit{this}}
	, m_inputField{new QLineEdit{this}}
	, m_sendButton{new QPushButton{tr("Send"), this}}
	, m_modelLabel{new QLabel{this}}
	, m_manager{new QNetworkAccessManager{this}}
	{
		auto *mainLayout = new QVBoxLayout{this};
		mainLayout->setContentsMargins(0, 0, 0, 0);
		mainLayout->setSpacing(0);
		
		// -- Header bar: same fixed height as sidebar header ---------------
		auto *headerWidget = new QWidget{this};
		headerWidget->setFixedHeight(40);
		auto *headerLayout = new QHBoxLayout{headerWidget};
		// Mirror the sidebar header's 8 px side margins.
		headerLayout->setContentsMargins(8, 0, 8, 0);
		headerLayout->setSpacing(8);
		
		headerLayout->addWidget(m_modelLabel);
		headerLayout->addStretch();
		
		auto *switchBtn = new QPushButton{tr("Switch model\xe2\x80\xa6"), this};
		headerLayout->addWidget(switchBtn);
		
		mainLayout->addWidget(headerWidget);
		
		// -- Native horizontal separator -----------------------------------
		auto *sep = new QFrame{this};
		sep->setFrameShape(QFrame::HLine);
		sep->setFrameShadow(QFrame::Sunken);
		mainLayout->addWidget(sep);
		
		// -- Chat display --------------------------------------------------
		m_chatBox->setReadOnly(true);
		m_chatBox->setFrameShape(QFrame::NoFrame);
		mainLayout->addWidget(m_chatBox, 1);
		
		// -- Input row -----------------------------------------------------
		auto *inputSep = new QFrame{this};
		inputSep->setFrameShape(QFrame::HLine);
		inputSep->setFrameShadow(QFrame::Sunken);
		mainLayout->addWidget(inputSep);
		
		auto *inputWidget = new QWidget{this};
		auto *inputLayout = new QHBoxLayout{inputWidget};
		inputLayout->setContentsMargins(8, 6, 8, 6);
		inputLayout->setSpacing(6);
		
		m_inputField->setPlaceholderText(tr("Type a message\xe2\x80\xa6"));
		inputLayout->addWidget(m_inputField);
		inputLayout->addWidget(m_sendButton);
		mainLayout->addWidget(inputWidget);
		
		connect(m_sendButton, &QPushButton::clicked,
				this,         &ChatPane::sendMessage);
		connect(m_inputField, &QLineEdit::returnPressed,
				this,         &ChatPane::sendMessage);
		connect(switchBtn,    &QPushButton::clicked,
				this,         &ChatPane::pickModel);
	}
	
	void loadSession(const Session &s)
	{
		m_session = s;
		rebuildChatBox();
		updateModelLabel();
	}
	
	[[nodiscard]] const Session &session() const { return m_session; }
	
	void setTitle(const QString &title) { m_session.title = title; }
	
signals:
	void sessionChanged(const Session &session);
	void modelChanged(const QString &model);
	
private slots:
	void sendMessage()
	{
		const QString userText = m_inputField->text().trimmed();
		if (userText.isEmpty()) return;
		
		if (m_activeReply && m_activeReply->isRunning())
			m_activeReply->abort();
		
		if (m_session.messages.isEmpty()) {
			m_session.title = userText.left(40) +
			(userText.size() > 40 ? QStringLiteral("\xe2\x80\xa6") : QString{});
		}
		
		m_session.messages.append({QStringLiteral("user"), userText});
		m_chatBox->append(QStringLiteral("<b>You:</b> ") + userText.toHtmlEscaped());
		m_inputField->clear();
		m_sendButton->setEnabled(false);
		
		m_session.updatedAt = QDateTime::currentDateTime();
		emit sessionChanged(m_session);
		
		QNetworkRequest request{
			QUrl{QString::fromLatin1(OllamaGui::kApiGenerateUrl.data(),
				static_cast<qsizetype>(OllamaGui::kApiGenerateUrl.size()))}};
				request.setHeader(QNetworkRequest::ContentTypeHeader,
								  QStringLiteral("application/json"));
				
				QJsonObject json;
				json[QStringLiteral("model")]  = m_session.model;
				json[QStringLiteral("prompt")] = userText;
				json[QStringLiteral("stream")] = true;
				
				m_activeReply = m_manager->post(request, QJsonDocument{json}.toJson());
				
				m_chatBox->append(QStringLiteral("<b>Bot:</b> "));
				{
					QTextCursor c = m_chatBox->textCursor();
					c.movePosition(QTextCursor::End);
					c.deletePreviousChar();
					m_chatBox->setTextCursor(c);
					m_botBlockAnchor = c.position();
				}
				m_currentBotText.clear();
				
				connect(m_activeReply, &QNetworkReply::readyRead,
						this,          &ChatPane::onReadyRead);
				connect(m_activeReply, &QNetworkReply::finished,
						this,          &ChatPane::onReplyFinished);
	}
	
	void onReadyRead()
	{
		if (!m_activeReply) return;
		while (m_activeReply->canReadLine()) {
			const QByteArray    line = m_activeReply->readLine().trimmed();
			if (line.isEmpty()) continue;
			const QJsonDocument doc  = QJsonDocument::fromJson(line);
			if (doc.isNull()) continue;
			const QString token = doc.object()[QStringLiteral("response")].toString();
			appendToken(token);
			m_currentBotText += token;
		}
	}
	
	void onReplyFinished()
	{
		if (!m_activeReply) return;
		
		while (m_activeReply->canReadLine()) {
			const QByteArray    line = m_activeReply->readLine().trimmed();
			if (line.isEmpty()) continue;
			const QJsonDocument doc  = QJsonDocument::fromJson(line);
			if (doc.isNull()) continue;
			const QString token = doc.object()[QStringLiteral("response")].toString();
			if (!token.isEmpty()) {
				appendToken(token);
				m_currentBotText += token;
			}
		}
		
		if (m_activeReply->error() != QNetworkReply::NoError &&
			m_activeReply->error() != QNetworkReply::OperationCanceledError)
		{
			const QString errMsg =
			QStringLiteral("[Error: ") + m_activeReply->errorString() +
			QStringLiteral("]");
			m_currentBotText += errMsg;
		}
		
		if (!m_currentBotText.isEmpty()) {
			QTextCursor cur = m_chatBox->textCursor();
			cur.setPosition(m_botBlockAnchor);
			cur.movePosition(QTextCursor::End, QTextCursor::KeepAnchor);
			cur.removeSelectedText();
			cur.insertHtml(markdownToHtml(m_currentBotText));
			m_chatBox->setTextCursor(cur);
		}
		
		{
			QTextCursor cur = m_chatBox->textCursor();
			cur.movePosition(QTextCursor::End);
			cur.insertText(QStringLiteral("\n"));
		}
		
		if (!m_currentBotText.isEmpty()) {
			m_session.messages.append({QStringLiteral("bot"), m_currentBotText});
			m_currentBotText.clear();
		}
		m_session.updatedAt = QDateTime::currentDateTime();
		emit sessionChanged(m_session);
		
		m_chatBox->verticalScrollBar()->setValue(
			m_chatBox->verticalScrollBar()->maximum());
		m_sendButton->setEnabled(true);
		m_activeReply->deleteLater();
		m_activeReply = nullptr;
	}
	
	void pickModel()
	{
		ModelPickerDialog dlg{this};
		if (dlg.exec() != QDialog::Accepted) return;
		m_session.model = dlg.selectedModel();
		updateModelLabel();
		m_chatBox->append(
			QStringLiteral("<i>\xe2\x80\x94 switched to ") +
			m_session.model.toHtmlEscaped() +
			QStringLiteral(" \xe2\x80\x94</i>"));
		m_session.updatedAt = QDateTime::currentDateTime();
		emit sessionChanged(m_session);
		emit modelChanged(m_session.model);
	}
	
private:
	void updateModelLabel()
	{
		m_modelLabel->setText(
			tr("Model: ") + m_session.model);
	}
	
	[[nodiscard]] static QString markdownToHtml(const QString &md)
	{
		QTextDocument doc;
		doc.setMarkdown(md);
		QString      html      = doc.toHtml();
		const qsizetype bodyStart = html.indexOf(QStringLiteral("<body"));
		const qsizetype bodyEnd   = html.lastIndexOf(QStringLiteral("</body>"));
		if (bodyStart != -1 && bodyEnd != -1) {
			const qsizetype contentStart =
			html.indexOf(QLatin1Char('>'), bodyStart) + 1;
			html = html.mid(contentStart, bodyEnd - contentStart).trimmed();
		}
		return html;
	}
	
	void appendToken(const QString &token)
	{
		QTextCursor cur = m_chatBox->textCursor();
		cur.movePosition(QTextCursor::End);
		cur.insertText(token);
		m_chatBox->verticalScrollBar()->setValue(
			m_chatBox->verticalScrollBar()->maximum());
	}
	
	void rebuildChatBox()
	{
		m_chatBox->clear();
		for (const auto &msg : m_session.messages) {
			if (msg.role == QStringLiteral("user")) {
				m_chatBox->append(
					QStringLiteral("<b>You:</b> ") + msg.text.toHtmlEscaped());
			} else {
				m_chatBox->append(QStringLiteral("<b>Bot:</b> "));
				{
					QTextCursor c = m_chatBox->textCursor();
					c.movePosition(QTextCursor::End);
					c.deletePreviousChar();
					c.insertHtml(markdownToHtml(msg.text));
					m_chatBox->setTextCursor(c);
				}
				QTextCursor c = m_chatBox->textCursor();
				c.movePosition(QTextCursor::End);
				c.insertText(QStringLiteral("\n"));
			}
		}
		m_chatBox->verticalScrollBar()->setValue(
			m_chatBox->verticalScrollBar()->maximum());
	}
	
	Session                m_session;
	QTextEdit             *m_chatBox       = nullptr;
	QLineEdit             *m_inputField    = nullptr;
	QPushButton           *m_sendButton    = nullptr;
	QLabel                *m_modelLabel    = nullptr;
	QNetworkAccessManager *m_manager       = nullptr;
	QNetworkReply         *m_activeReply   = nullptr;
	QString                m_currentBotText;
	int                    m_botBlockAnchor = 0;
};

// ---------------------------------------------------------------------------
// Main window  —  sidebar + chat pane
// ---------------------------------------------------------------------------

class MainWindow final : public QWidget {
	Q_OBJECT
	
public:
	explicit MainWindow(QString initialModel, QWidget *parent = nullptr)
	: QWidget{parent}
	, m_store{}
	, m_sidebar{new SessionSidebar{&m_store, this}}
	, m_chatPane{new ChatPane{this}}
	, m_initialModel{std::move(initialModel)}
	{
		setWindowTitle(QStringLiteral("Ollama"));
		resize(OllamaGui::kWindowWidth, OllamaGui::kWindowHeight);
		
		auto *rootLayout = new QHBoxLayout{this};
		rootLayout->setContentsMargins(0, 0, 0, 0);
		rootLayout->setSpacing(0);
		
		rootLayout->addWidget(m_sidebar);
		
		// Native vertical separator between sidebar and chat pane
		auto *divider = new QFrame{this};
		divider->setFrameShape(QFrame::VLine);
		divider->setFrameShadow(QFrame::Sunken);
		rootLayout->addWidget(divider);
		
		rootLayout->addWidget(m_chatPane, 1);
		
		connect(m_sidebar,  &SessionSidebar::sessionSelected,
				this,       &MainWindow::onSessionSelected);
		connect(m_sidebar,  &SessionSidebar::newSessionRequested,
				this,       &MainWindow::onNewSession);
		connect(m_sidebar,  &SessionSidebar::renameRequested,
				this,       &MainWindow::onRenameSession);
		connect(m_sidebar,  &SessionSidebar::deleteRequested,
				this,       &MainWindow::onDeleteSession);
		connect(m_chatPane, &ChatPane::sessionChanged,
				this,       &MainWindow::onSessionChanged);
		
		const QVector<Session> sessions = m_store.loadAll();
		if (sessions.isEmpty()) {
			createAndLoadNewSession();
		} else {
			loadSession(sessions.first());
			m_sidebar->setActiveId(sessions.first().id);
		}
	}
	
private slots:
	void onNewSession() { createAndLoadNewSession(); }
	
	void onSessionSelected(const QString &id)
	{
		// Don't do anything if it's already the active session
		// (sidebar reload after delete can re-trigger this with the same id).
		if (id == m_currentSessionId) return;
		
		// Only save if the current session still exists on disk
		// (it may have just been deleted, in which case we must not re-save it).
		if (!m_currentSessionId.isEmpty()) {
			const QVector<Session> existing = m_store.loadAll();
			const bool stillExists = std::any_of(existing.cbegin(), existing.cend(),
												 [&](const Session &s) { return s.id == m_currentSessionId; });
			if (stillExists)
				m_store.save(m_chatPane->session());
		}
		
		const QVector<Session> sessions = m_store.loadAll();
		for (const auto &s : sessions) {
			if (s.id == id) {
				loadSession(s);
				return;
			}
		}
	}
	
	void onSessionChanged(const Session &session)
	{
		m_store.save(session);
		m_sidebar->reload(session.id);
	}
	
	void onRenameSession(const QString &id, const QString &newTitle)
	{
		QVector<Session> sessions = m_store.loadAll();
		for (auto &s : sessions) {
			if (s.id == id) {
				s.title = newTitle;
				m_store.save(s);
				if (s.id == m_currentSessionId)
					m_chatPane->setTitle(newTitle);
				break;
			}
		}
		m_sidebar->reload(m_currentSessionId);
	}
	
	void onDeleteSession(const QString &id)
	{
		m_store.remove(id);
		
		// Clear tracked id BEFORE reload() so that any sessionSelected signal
		// fired during sidebar rebuild doesn't attempt to save the deleted session.
		if (id == m_currentSessionId)
			m_currentSessionId.clear();
		
		const QVector<Session> remaining = m_store.loadAll();
		if (remaining.isEmpty()) {
			createAndLoadNewSession();
		} else {
			// Load the session into the chat pane first, then sync the sidebar.
			// If we did reload() first it would emit sessionSelected which would
			// call loadSession again — causing a double-load and potential flicker.
			const bool needsNewPane = m_currentSessionId.isEmpty();
			if (needsNewPane)
				loadSession(remaining.first());
			m_sidebar->reload(remaining.first().id);
		}
	}
	
private:
	void createAndLoadNewSession()
	{
		Session s = Session::create(m_initialModel);
		m_store.save(s);
		loadSession(s);
		m_sidebar->reload(s.id);
	}
	
	void loadSession(const Session &s)
	{
		m_currentSessionId = s.id;
		m_chatPane->loadSession(s);
		setWindowTitle(QStringLiteral("Ollama \xe2\x80\x94 ") + s.title);
	}
	
	SessionStore   m_store;
	SessionSidebar *m_sidebar;
	ChatPane       *m_chatPane;
	QString         m_initialModel;
	QString         m_currentSessionId;
};

// ---------------------------------------------------------------------------
// Helpers — Ollama process management
// ---------------------------------------------------------------------------

namespace OllamaGui {
	
	[[nodiscard]] inline bool isOllamaRunning()
	{
		QTcpSocket sock;
		sock.connectToHost(QStringLiteral("127.0.0.1"), 11434);
		return sock.waitForConnected(300);
	}
	
	[[nodiscard]] inline bool startOllama()
	{
		return QProcess::startDetached(QStringLiteral("ollama"),
									   QStringList{QStringLiteral("serve")});
	}
	
	[[nodiscard]] inline bool waitForOllama(int maxWaitMs  = 8000,
											int intervalMs = 300)
	{
		const int steps = maxWaitMs / intervalMs;
		for (int i = 0; i < steps; ++i) {
			if (isOllamaRunning()) return true;
			QThread::msleep(static_cast<unsigned long>(intervalMs));
		}
		return false;
	}
	
} // namespace OllamaGui

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

#include "main.moc"

int main(int argc, char *argv[])
{
	QApplication app{argc, argv};
	
	// -- 1. Ensure Ollama is running ----------------------------------------
	if (!OllamaGui::isOllamaRunning()) {
		if (!OllamaGui::startOllama()) {
			QMessageBox::critical(
				nullptr,
				QApplication::translate("main", "Ollama not found"),
								  QApplication::translate(
									  "main",
								  "Could not start Ollama. "
								  "Make sure the \xe2\x80\x98ollama\xe2\x80\x99 binary is on your PATH "
								  "and try again."));
			return 1;
		}
		if (!OllamaGui::waitForOllama()) {
			QMessageBox::warning(
				nullptr,
				QApplication::translate("main", "Ollama slow to start"),
								 QApplication::translate(
									 "main",
								 "Ollama was launched but did not respond within 8 seconds. "
								 "The model picker may show no models — "
								 "you can still type a model name manually."));
		}
	}
	
	// -- 2. Pick model ------------------------------------------------------
	QString model;
	
	if (argc > 1) {
		model = QString::fromLocal8Bit(argv[1]);
	} else {
		ModelPickerDialog picker;
		if (picker.exec() != QDialog::Accepted)
			return 0;
		model = picker.selectedModel();
	}
	
	// -- 3. Open main window ------------------------------------------------
	MainWindow window{std::move(model)};
	window.show();
	
	return app.exec();
}
