package com.inik.phototest2.model

data class GphotoWidget(
    val name: String,
    val label: String,
    val type: String,
    val choices: List<String> = emptyList(),
    val children: List<GphotoWidget> = emptyList()
)