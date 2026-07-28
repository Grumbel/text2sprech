// SPDX-FileCopyrightText: 2026 Ingo Ruhnke <grumbel@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RibbonBar.h"

#include <QAbstractSpinBox>
#include <QAction>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QSizePolicy>
#include <QToolButton>

namespace {
// Every interactive control in the bar shares this height, so buttons,
// combo boxes, and spin boxes all line up on one baseline.
constexpr int kControlHeight = 30;
constexpr int kIconSize = 18;
// Height of the vertical rule between groups; shorter than the bar itself
// so it reads as a subtle divider, not a hard wall.
constexpr int kSeparatorHeight = 22;
} // namespace

RibbonGroup::RibbonGroup(const QString &title, QWidget *parent)
    : QWidget(parent)
    , m_title(title)
{
    setObjectName(QStringLiteral("ribbonGroup"));
    setAccessibleName(title);
    setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);

    m_controls = new QHBoxLayout(this);
    m_controls->setContentsMargins(0, 0, 0, 0);
    m_controls->setSpacing(4);
    m_controls->setAlignment(Qt::AlignVCenter);
}

QToolButton *RibbonGroup::addAction(QAction *action, Qt::ToolButtonStyle style)
{
    auto *btn = new QToolButton(this);
    btn->setDefaultAction(action);
    btn->setToolButtonStyle(style);
    btn->setAutoRaise(true);
    btn->setIconSize(QSize(kIconSize, kIconSize));
    btn->setFixedHeight(kControlHeight);
    btn->setCursor(Qt::PointingHandCursor);
    if (!m_title.isEmpty() && action) {
        // Keep the group's purpose discoverable without drawing a caption
        // row: "Voice — Speed" instead of just "Speed".
        const QString tip = action->toolTip();
        btn->setToolTip(tip.isEmpty() ? m_title : (m_title + QStringLiteral(" — ") + tip));
    }
    m_controls->addWidget(btn, 0, Qt::AlignVCenter);
    return btn;
}

void RibbonGroup::addWidget(QWidget *widget, int stretch)
{
    if (!widget) {
        return;
    }
    widget->setParent(this);
    // Normalize height for the common "other" controls used in the bar so
    // they sit on the same baseline as the tool buttons.
    if (qobject_cast<QComboBox *>(widget) || qobject_cast<QAbstractSpinBox *>(widget)) {
        widget->setFixedHeight(kControlHeight);
    }
    m_controls->addWidget(widget, stretch, Qt::AlignVCenter);
}

void RibbonGroup::addSpacing(int px)
{
    m_controls->addSpacing(px);
}

RibbonBar::RibbonBar(QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("ribbonBar"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setFixedHeight(48);

    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(12, 0, 12, 0);
    m_layout->setSpacing(10);
    m_layout->setAlignment(Qt::AlignVCenter);

    setStyleSheet(QStringLiteral(
        "QWidget#ribbonBar {"
        "  background-color: palette(window);"
        "  border-bottom: 1px solid palette(mid);"
        "}"
        "QWidget#ribbonGroup {"
        "  background: transparent;"
        "}"
        "QToolButton {"
        "  padding: 4px 8px;"
        "  border: 1px solid transparent;"
        "  border-radius: 5px;"
        "}"
        "QToolButton:hover {"
        "  background-color: palette(midlight);"
        "}"
        "QToolButton:pressed {"
        "  background-color: palette(mid);"
        "}"
        "QToolButton:checked {"
        "  background-color: palette(highlight);"
        "  color: palette(highlighted-text);"
        "}"
        "QToolButton::menu-button {"
        "  border: none;"
        "  width: 14px;"
        "}"
        "QFrame#ribbonSeparator {"
        "  background-color: palette(mid);"
        "  max-width: 1px;"
        "  min-width: 1px;"
        "}"
        "QComboBox, QDoubleSpinBox {"
        "  padding: 2px 6px;"
        "  border: 1px solid palette(mid);"
        "  border-radius: 5px;"
        "  background-color: palette(base);"
        "}"));
}

void RibbonBar::appendSeparator()
{
    auto *sep = new QFrame(this);
    sep->setObjectName(QStringLiteral("ribbonSeparator"));
    sep->setFrameShape(QFrame::VLine);
    sep->setFrameShadow(QFrame::Plain);
    sep->setFixedWidth(1);
    sep->setFixedHeight(kSeparatorHeight);
    m_layout->addWidget(sep, 0, Qt::AlignVCenter);
}

RibbonGroup *RibbonBar::addGroup(const QString &title)
{
    if (m_hasGroup) {
        appendSeparator();
    }
    auto *group = new RibbonGroup(title, this);
    m_layout->addWidget(group, 0, Qt::AlignVCenter);
    m_hasGroup = true;
    return group;
}

void RibbonBar::addStretch(int stretch)
{
    m_layout->addStretch(stretch);
}

QSize RibbonBar::sizeHint() const
{
    const QSize base = QWidget::sizeHint();
    return QSize(base.width(), qMax(base.height(), 48));
}
