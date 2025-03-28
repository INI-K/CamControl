import android.util.Log
import com.inik.phototest2.model.GphotoWidget
import org.json.JSONArray
import org.json.JSONObject

fun parseWidgetJson(obj: JSONObject): GphotoWidget {

    val name = obj.optString("name", "")
    val label = obj.optString("label", "")
    val type = obj.optString("type", "UNKNOWN")

    // choices 배열
    val choicesArray = obj.optJSONArray("choices") ?: JSONArray()
    val choicesList = mutableListOf<String>()
    for (i in 0 until choicesArray.length()) {
        choicesList.add(choicesArray.getString(i))
    }

    // children 배열
    val childrenArray = obj.optJSONArray("children") ?: JSONArray()
    val childList = mutableListOf<GphotoWidget>()
    for (i in 0 until childrenArray.length()) {
        val childObj = childrenArray.getJSONObject(i)
        val childWidget = parseWidgetJson(childObj)
        childList.add(childWidget)
    }

    return GphotoWidget(
        name = name,
        label = label,
        type = type,
        choices = choicesList,
        children = childList
    )
}