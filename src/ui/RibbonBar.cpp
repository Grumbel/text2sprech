// SPDX-FileCopyrightText: 2026 Ingo Ruhnke <grumbel@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#include "RibbonBar.h"

#include <QAction>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QSizePolicy>
#include <QToolButton>
#include <QVBoxLayout>

RibbonGroup::RibbonGroup(const QString &title, QWidget *parent)
    : QWidget(parent)
{
    setObjectName(QStringLiteral("ribbonGroup"));
    // Fill the bar height so every group's title sits on the same baseline.
    setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Expanding);

    auto *outer = new QVBoxLayout(this);
    outer->setContentsMargins(10, 6, 10, 4);
    outer->setSpacing(2);

    m_controls = new QHBoxLayout;
    m_controls->setContentsMargins(0, 0, 0, 0);
    m_controls->setSpacing(4);
    // Controls stick to the top; empty space pushes the title to the bottom.
    m_controls->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    outer->addLayout(m_controls, /*stretch=*/1);

    m_titleLabel = new QLabel(title, this);
    m_titleLabel->setObjectName(QStringLiteral("ribbonGroupTitle"));
    m_titleLabel->setAlignment(Qt::AlignHCenter | Qt::AlignBottom);
    // Fixed title row so captions line up across groups.
    m_titleLabel->setMinimumHeight(16);
    outer->addWidget(m_titleLabel, 0, Qt::AlignHCenter | Qt::AlignBottom);
}

QToolButton *RibbonGroup::addAction(QAction *action, Qt::ToolButtonStyle style)
{
    auto *btn = new QToolButton(this);
    btn->setDefaultAction(action);
    btn->setToolButtonStyle(style);
    btn->setAutoRaise(true);
    btn->setIconSize(QSize(22, 22));
    btn->setMinimumWidth(52);
    m_controls->addWidget(btn, 0, Qt::AlignLeft | Qt::AlignTop);
    return btn;
}

void RibbonGroup::addWidget(QWidget *widget, int stretch)
{
    if (!widget) {
        return;
    }
    widget->setParent(this);
    m_controls->addWidget(widget, stretch, Qt::AlignLeft | Qt::AlignTop);
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
    // Fixed strip height so Expanding groups all match and titles align.
    setMinimumHeight(78);
    setMaximumHeight(96);

    m_layout = new QHBoxLayout(this);
    m_layout->setContentsMargins(6, 2, 6, 2);
    m_layout->setSpacing(0);
    // Default vertical alignment (not AlignTop) so Expanding groups stretch
    // to the full bar height and their bottom titles share one baseline.
    m_layout->setAlignment(Qt::AlignLeft);

    setStyleSheet(QStringLiteral(
        "QWidget#ribbonBar {"
        "  background-color: palette(window);"
        "  border-bottom: 1px solid palette(mid);"
        "}"
        "QWidget#ribbonGroup {"
        "  background: transparent;"
        "}"
        "QLabel#ribbonGroupTitle {"
        "  color: palette(mid);"
        "  font-size: 11px;"
        "  padding: 2px 4px 0 4px;"
        "}"
        "QToolButton {"
        "  padding: 4px 8px;"
        "  border: 1px solid transparent;"
        "  border-radius: 3px;"
        "}"
        "QToolButton:hover {"
        "  background-color: palette(midlight);"
        "  border: 1px solid palette(mid);"
        "}"
        "QToolButton:pressed, QToolButton:checked {"
        "  background-color: palette(light);"
        "  border: 1px solid palette(mid);"
        "}"
        "QFrame#ribbonSeparator {"
        "  max-width: 1px;"
        "  margin: 6px 4px;"
        "}"));
}

void RibbonBar::appendSeparator()
{
    auto *sep = new QFrame(this);
    sep->setObjectName(QStringLiteral("ribbonSeparator"));
    sep->setFrameShape(QFrame::VLine);
    sep->setFrameShadow(QFrame::Plain);
    sep->setFixedWidth(1);
    m_layout->addWidget(sep);
}

RibbonGroup *RibbonBar::addGroup(const QString &title)
{
    if (m_hasGroup) {
        appendSeparator();
    }
    auto *group = new RibbonGroup(title, this);
    // No AlignTop: Expanding groups fill the bar height so titles align.
    m_layout->addWidget(group, 0);
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
    return QSize(base.width(), qMax(base.height(), 78));
}
