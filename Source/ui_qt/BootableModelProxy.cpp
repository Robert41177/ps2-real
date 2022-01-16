#include "BootableModelProxy.h"
#include "BootableModel.h"
#include <regex>
BootableModelProxy::BootableModelProxy(QObject* parent)
    : QSortFilterProxyModel(parent)
{
}

bool BootableModelProxy::filterAcceptsRow(int sourceRow, const QModelIndex& sourceParent) const
{
	QModelIndex index = sourceModel()->index(sourceRow, 0, sourceParent);

	QVariant data = sourceModel()->data(index);
	if(data.canConvert<BootableCoverQVarient>())
	{
		BootableCoverQVarient bootablecover = qvariant_cast<BootableCoverQVarient>(data);
		QString key = QString::fromStdString(bootablecover.GetKey());
		QString title = QString::fromStdString(bootablecover.GetTitle());
		QString path = QString::fromStdString(bootablecover.GetPath());
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
		QRegularExpression regex = filterRegularExpression();
		regex.setPatternOptions(QRegularExpression::CaseInsensitiveOption);
#else
		// QRegExp is deprecated in Qt6, while QRegularExpression is recommended over QRegExp since Qt5
		// however QRegularExpression is producing incorrect results in Qt5
		QRegExp regex = filterRegExp();
		regex.setCaseSensitivity(Qt::CaseInsensitive);
#endif

		return key.contains(regex) || title.contains(regex) || path.contains(regex);
	}
	return false;
}