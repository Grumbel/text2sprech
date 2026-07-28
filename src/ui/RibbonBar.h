// SPDX-FileCopyrightText: 2026 Ingo Ruhnke <grumbel@gmail.com>
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <QWidget>

class QAction;
class QHBoxLayout;
class QLabel;
class QToolButton;

// A cluster of related controls (e.g. "Open + Paste", "Voice + Speed").
// Everything in a group sits on one shared, vertically-centered baseline —
// no separate caption row, no per-group height quirks. The group's title is
// exposed as an accessible name / tooltip prefix rather than drawn on
// screen, which is what kept the old ribbon-style strip from lining up.
class RibbonGroup : public QWidget {
    Q_OBJECT
public:
    explicit RibbonGroup(const QString &title, QWidget *parent = nullptr);

    QToolButton *addAction(QAction *action,
                           Qt::ToolButtonStyle style = Qt::ToolButtonTextBesideIcon);
    void addWidget(QWidget *widget, int stretch = 0);
    void addSpacing(int px = 6);

    QHBoxLayout *controlsLayout() const { return m_controls; }

private:
    QHBoxLayout *m_controls = nullptr;
    QString m_title;
};

// Single-row toolbar strip of logically grouped controls, separated by
// thin vertical rules. Every control shares one fixed-height, vertically
// centered baseline regardless of whether it's a button, combo box, or
// spin box.
class RibbonBar : public QWidget {
    Q_OBJECT
public:
    explicit RibbonBar(QWidget *parent = nullptr);

    RibbonGroup *addGroup(const QString &title);
    void addStretch(int stretch = 1);

    QSize sizeHint() const override;

private:
    void appendSeparator();

    QHBoxLayout *m_layout = nullptr;
    bool m_hasGroup = false;
};
