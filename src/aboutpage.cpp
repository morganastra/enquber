#include "aboutpage.h"

#include "theme.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFont>
#include <QLabel>
#include <QPalette>
#include <QVBoxLayout>

namespace {

constexpr int kIconSize = 72;

/// The text column is pinned so word-wrapped labels get the height that matches
/// the width they are actually laid out at (otherwise Qt sizes them for a wider
/// hint and clips the last lines).
constexpr int kContentWidth = 420;

const char *const kRepoUrl = "https://github.com/morganastra/enquber";
const char *const kQtUrl = "https://qt.io";
const char *const kQrencodeUrl = "https://github.com/fukuchi/libqrencode";
const char *const kGplUrl = "https://www.gnu.org/licenses/gpl-3.0.html";
const char *const kFeatherUrl = "https://feathericons.com";
const char *const kFeatherLicenseUrl = "https://github.com/feathericons/feather/blob/main/LICENSE";

QLabel *makeLabel(QWidget *parent, const QString &text)
{
    auto *label = new QLabel(text, parent);
    label->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    return label;
}

/// Dims a label the same way the status line and the drop-zone hint do. The
/// placeholder role is resolved when the label paints rather than frozen into
/// an explicit palette, so the text follows a light/dark theme switch.
void mute(QLabel *label)
{
    label->setForegroundRole(QPalette::PlaceholderText);
}

QFont scaled(const QFont &base, qreal factor)
{
    QFont font = base;
    font.setPointSizeF(base.pointSizeF() * factor);
    return font;
}

/// Pins a word-wrapped label to the height it needs at @p width. Without this
/// a short window lets the layout squeeze the label down to a single (clipped)
/// line, because a wrapped QLabel's minimumSizeHint is only one line tall.
void pinHeight(QLabel *label, int width)
{
    const int height = label->heightForWidth(width);
    if (height > 0) {
        label->setMinimumHeight(height);
    }
}

} // namespace

AboutPage::AboutPage(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("aboutPage"));

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 12, 20, 12);
    layout->setSpacing(6);
    layout->addStretch(1);

    m_icon = new QLabel(this);
    m_icon->setAlignment(Qt::AlignCenter);
    // theme::appIcon() is the icon bundled with enquber, so the page keeps its
    // picture even when the session's icon theme has no application entry.
    const QIcon appIcon = theme::appIcon();
    if (appIcon.isNull()) {
        m_icon->hide();
    } else {
        m_icon->setPixmap(appIcon.pixmap(QSize(kIconSize, kIconSize), devicePixelRatioF()));
    }
    layout->addWidget(m_icon);
    layout->addSpacing(2);

    //@ App
    //% "Enquber"
    auto *title = makeLabel(this, qtTrId("app.name"));
    title->setObjectName(QStringLiteral("aboutTitle"));
    QFont titleFont = scaled(title->font(), 1.5);
    titleFont.setWeight(QFont::Bold);
    title->setFont(titleFont);
    layout->addWidget(title);

    const QString version = QApplication::applicationVersion();
    if (!version.isEmpty()) {
        //@ AboutPage
        //% "Version %1"
        auto *versionLabel = makeLabel(this, qtTrId("about.version").arg(version));
        versionLabel->setObjectName(QStringLiteral("aboutVersion"));
        mute(versionLabel);
        layout->addWidget(versionLabel);
    }

    layout->addSpacing(4);
    //@ AboutPage
    //% "A simple QR code maker"
    layout->addWidget(makeLabel(this, qtTrId("about.tagline")));
    layout->addSpacing(10);

    //@ AboutPage
    //% "Copyright © 2026 Morgan Astra"
    auto *copyright = makeLabel(this, qtTrId("about.copyright"));
    copyright->setObjectName(QStringLiteral("aboutCopyright"));
    layout->addWidget(copyright);

    layout->addSpacing(4);
    //@ AboutPage
    //% "Enquber is free software: you can redistribute it and/or modify it under the terms of the GNU General Public License, version 3 or later. It is distributed in the hope that it will be useful, but without any warranty."
    auto *licence = makeLabel(this, qtTrId("about.licence"));
    licence->setObjectName(QStringLiteral("aboutLicence"));
    licence->setFont(scaled(licence->font(), 0.92));
    licence->setFixedWidth(kContentWidth);
    mute(licence);
    pinHeight(licence, kContentWidth);
    layout->addWidget(licence, 0, Qt::AlignHCenter);

    layout->addSpacing(12);
    auto *links = new QLabel(this);
    links->setObjectName(QStringLiteral("aboutLinks"));
    links->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    links->setWordWrap(true);
    links->setTextFormat(Qt::RichText);
    links->setOpenExternalLinks(true);
    links->setTextInteractionFlags(Qt::TextBrowserInteraction);
    links->setFixedWidth(kContentWidth);
    //@ AboutPage
    //% "Project source: <a href=\"%1\">github.com/morganastra/enquber</a><br><br>Built with <a href=\"%2\">Qt</a> and <a href=\"%3\">libqrencode</a>.<br><br><a href=\"%4\">Feather Icons</a> © 2013–2023 Cole Bemis, <a href=\"%5\">MIT license</a><br><br><a href=\"%6\">GNU GPL v3 full text</a>"
    links->setText(qtTrId("about.links")
                       .arg(QString::fromLatin1(kRepoUrl), QString::fromLatin1(kQtUrl),
                            QString::fromLatin1(kQrencodeUrl), QString::fromLatin1(kFeatherUrl),
                            QString::fromLatin1(kFeatherLicenseUrl), QString::fromLatin1(kGplUrl)));
    pinHeight(links, kContentWidth);
    layout->addWidget(links, 0, Qt::AlignHCenter);

    layout->addStretch(1);

    //@ AboutPage
    //% "Press Esc to go back"
    auto *hint = makeLabel(this, qtTrId("about.hint"));
    hint->setObjectName(QStringLiteral("aboutHint"));
    hint->setFont(scaled(hint->font(), 0.92));
    mute(hint);
    layout->addWidget(hint);
}
