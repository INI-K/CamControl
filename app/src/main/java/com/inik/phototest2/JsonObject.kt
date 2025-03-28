// app/src/main/java/parseWidgetJson.kt

import org.json.JSONArray
import org.json.JSONObject
import com.inik.phototest2.model.GphotoWidget

fun parseWidgetJson(obj: JSONObject): GphotoWidget {
    val name = obj.optString("name", "")
    val label = obj.optString("label", "")
    val type = obj.optString("type", "UNKNOWN")

    // choices
    val choicesList = mutableListOf<String>()
    if (obj.has("choices")) {
        val choices = obj.optJSONArray("choices") ?: JSONArray()
        for (i in 0 until choices.length()) {
            choicesList.add(choices.optString(i, ""))
        }
    }

    // children
    val childrenList = mutableListOf<GphotoWidget>()
    if (obj.has("children")) {
        val children = obj.optJSONArray("children") ?: JSONArray()
        for (i in 0 until children.length()) {
            val childObj = children.optJSONObject(i) ?: continue
            val childWidget = parseWidgetJson(childObj)
            childrenList.add(childWidget)
        }
    }

    return GphotoWidget(
        name = name,
        label = label,
        type = type,
        choices = choicesList,
        children = childrenList
    )
}